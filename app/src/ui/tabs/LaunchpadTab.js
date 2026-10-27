// The Launchpad: the panel the module has not got, on a cable it has.
//
// Three panels. **The Launchpad** is the patch's: which cable it is on,
// which model, and how its note page plays - all of it `LaunchpadConfig`
// (src/node/patch.h), written over the protocol like any other setting, and
// travelling with the patch because a patch is played from its panel.
// **The pads** are the page's: the device on the other end of that cable,
// drawn from the LEDs the module lit and pressable (ui/launchpad/Launchpad.js),
// standing in for the Mini, the X or the Pro - or standing aside for a real
// one plugged into this computer. **The widgets** are the patch's again: what
// each column of the two custom pages sends.
//
// Nothing here knows what a page shows or what a pad plays. The firmware does
// (src/surface/launchpad.h); this tab sets it up and watches it happen.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { Select, range } from '../components/Select.js';
import { NumberField } from '../components/NumberField.js';
import { Segmented } from '../components/Segmented.js';
import {
  MUSICAL_PORTS, LAUNCHPAD_MODELS, LP_WIDGET_KINDS, LP_COLOURS, launchpadModelName, portNames, channelLabel,
} from '../../protocol/names.js';
import { describeSupport } from '../../runtime/webmidi.js';
import { MODELS, paletteColour } from '../../runtime/launchpad.js';
import { emptyLaunchpad, isWidget } from '../../protocol/codec.js';
import { DEFAULT_KEY_OCTAVE, MAX_KEY_OCTAVE } from '../../core/music.js';
import { LaunchpadView } from '../launchpad/Launchpad.js';
import '../launchpad/Launchpad.css';

// The firmware's page names, in its Page order (src/surface/launchpad.h).
const PAGES = ['note', 'key', 'widgets', 'tempo', 'audio'];
const WIDGET_COLUMNS = 8;

export function LaunchpadTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  return el('div', {}, ConfigPanel(app), PadsPanel(app), WidgetsPanel(app));
}

const configOf = (app) => app.state.patch.launchpad ?? emptyLaunchpad();

// --- the patch's half ------------------------------------------------------------

export function ConfigPanel(app) {
  const lp = configOf(app);
  const set = (changes) => app.editor.setLaunchpad(changes);
  return Panel('Launchpad',
    Fields(
      Field({ label: 'on cable', hint: 'USB host Launchpad for one on the module’s USB host socket, or a DIN port for a Pro' }, Select({
        options: [{ value: 0, label: 'none' }, ...MUSICAL_PORTS],
        value: lp.port, 'aria-label': 'the cable the Launchpad is on',
        onChange: (port) => set({ port }),
      })),
      Field({ label: 'model' }, Select({
        options: LAUNCHPAD_MODELS, value: lp.model, 'aria-label': 'which Launchpad',
        onChange: (model) => set({ model }),
      }))),
    lp.port ? Fields(
      Field({ label: 'notes play on' }, Select({
        options: range(17, (c) => channelLabel(c), 1), value: lp.channel || 1,
        'aria-label': 'the channel the note page plays on',
        onChange: (channel) => set({ channel }),
      })),
      Field({ label: 'register', hint: 'where the bottom-left pad plays' }, Select({
        options: [{ value: 0, label: `the key’s (${DEFAULT_KEY_OCTAVE})` },
                  ...range(MAX_KEY_OCTAVE + 1, (o) => `register ${o}`, 1)],
        value: lp.octave, 'aria-label': 'the note page’s register',
        onChange: (octave) => set({ octave }),
      }))) : null,
    StateLine(app, lp),
    Hint(lp.port
      ? 'The top row’s four right-hand buttons - Session to Custom on a Pro - are the pages: key, note, widgets, tempo. '
        + 'The right column is start/stop, continue, tap, panic and the audio page on every page. '
        + 'On the audio page left and right step through the rack’s slots, up and down through their parameters, '
        + 'and the bottom of the right column empties the slot; a Pro’s upper bottom row picks a slot from anywhere. '
        + 'A note-page pad plays the key; a widget’s CC arrives on this cable, so bind it in the mod matrix or learn it.'
      : 'Choose a cable and the module asks it who is there. What answers is put into programmer mode and lit.'));
}

// What the module says its surface is doing: linked or waiting, which model
// answered, which page. Polled while this line is on screen, like a live
// global (services/session.js).
function StateLine(app, lp) {
  if (!lp.port) return null;
  app.live.watchLaunchpad();
  const line = el('p', { class: 'lp-status' }, 'asking the cable…');
  const paint = () => {
    const state = app.session.lpLive;
    if (!state) { line.textContent = `waiting for a Launchpad on ${portNames(lp.port).join(', ')}`; line.classList.remove('linked'); return; }
    if (!state.linked) {
      line.textContent = `no Launchpad has answered on ${portNames(lp.port).join(', ')}`;
      line.classList.remove('linked');
      return;
    }
    const shift = state.octaveShift ? ` · ${state.octaveShift > 0 ? '+' : ''}${state.octaveShift} oct` : '';
    const page = PAGES[state.page] ?? `page ${state.page}`;
    const widgets = state.page === 2 ? ` ${state.widgetPage + 1}`
      : state.page === 4 ? ` · slot ${state.audioSlot + 1}, parameters page ${state.audioPage + 1}` : '';
    line.textContent = `linked · ${launchpadModelName(state.model)} · ${page} page${widgets}${shift}`;
    line.classList.add('linked');
  };
  paint();
  app.live.paint(paint);
  return line;
}

// --- the page's half -------------------------------------------------------------

export function PadsPanel(app) {
  const lp = configOf(app);
  const virtual = app.launchpad;
  const title = 'the pads';
  if (!app.session.usingModule || !virtual) {
    return Panel(title, Hint(app.hosted
      ? 'the plugin’s Launchpad is whatever the DAW routes to its MIDI input; the plugin lights it, not this page'
      : 'a module on the cable has its Launchpad on its own socket; this page cannot see that wire'));
  }
  if (!lp.port) return Panel(title, Hint('no Launchpad in this patch: choose a cable above'));

  const bridged = Boolean(virtual.bridged);
  return Panel(title,
    el('div', { class: 'lp-stage' },
      Segmented({
        label: 'which Launchpad the page stands in for',
        options: Object.entries(MODELS).map(([value, spec]) => ({ value, label: spec.label.replace('Launchpad ', '') })),
        value: virtual.model,
        onChange: (model) => { if (!bridged) { virtual.setModel(model); app.render(); } },
      }),
      LaunchpadView(app, virtual),
      DrawingLine(app, virtual, lp)),
    HardwareRow(app, virtual));
}

// What the drawing is showing. Painted rather than rendered: the module puts
// the device into programmer mode passes after the tab was drawn.
function DrawingLine(app, virtual, lp) {
  const line = el('p', { class: 'lp-status' });
  const paint = () => {
    line.textContent = virtual.bridged ? `${virtual.bridged} is on ${portNames(lp.port).join(', ')}; the drawing follows it`
      : virtual.programmer ? 'programmer mode: every light here is one the module lit'
      : 'the module has not put this Launchpad into programmer mode yet';
  };
  paint();
  app.live.paint(paint);
  return line;
}

// A real Launchpad plugged into this computer, on the module's cable in the
// drawing's place.
function HardwareRow(app, virtual) {
  if (!virtual.access) {
    const support = describeSupport();
    return support.ok
      ? Row(el('button', { onclick: async () => { try { await virtual.connect(); } catch { /* refused */ } app.render(); } },
               'play from a real Launchpad'))
      : Hint(`a real Launchpad needs Web MIDI: ${support.reason}`);
  }
  const devices = virtual.devices();
  if (!devices.length) return Hint('no Launchpad is plugged into this computer');
  return Fields(Field({ label: 'a real Launchpad', hint: 'bridged onto the module’s cable' }, Select({
    class: 'grow', 'aria-label': 'which Launchpad on this computer plays the module',
    options: [{ value: '', label: 'none — the drawing above' }, ...devices.map((d) => ({ value: d.id, label: d.name }))],
    value: virtual.hardwareInput?.id ?? '',
    onChange: (id) => { if (id) virtual.bridge(id); else virtual.unbridge(); app.render(); },
  })));
}

// --- the widgets -------------------------------------------------------------------

export function WidgetsPanel(app) {
  const lp = configOf(app);
  const caps = app.device.capabilities;
  const count = Math.min(caps.lpWidgets ?? P.LP_WIDGETS, lp.widgets.length);
  if (!lp.port) return null;
  const pages = Math.ceil(count / WIDGET_COLUMNS);
  const rows = [];
  for (let page = 0; page < pages; page++) {
    rows.push(el('div', { class: 'lp-page-head' },
      el('h3', {}, `page ${page + 1}`),
      el('span', {}, page === 0 ? 'the widgets page as it opens' : 'the right arrow turns to it')));
    for (let col = 0; col < WIDGET_COLUMNS && page * WIDGET_COLUMNS + col < count; col++) {
      rows.push(WidgetRow(app, lp, page * WIDGET_COLUMNS + col, col));
    }
  }
  return Panel('widgets',
    Hint('Each column of the custom pages sends a CC into the patch on this cable: a fader is eight '
       + 'levels of one CC, buttons and toggles are eight CCs from the first up. Bind one in the mod '
       + 'matrix, or arm learn beside a parameter and press the column.'),
    el('div', { class: 'lp-widgets' }, rows));
}

function WidgetRow(app, lp, slot, col) {
  const widget = lp.widgets[slot];
  const g = isWidget(widget) ? widget : { kind: P.LaunchpadWidgetKind.LP_WIDGET_NONE, cc: 20 + slot, channel: 1, colour: 0 };
  const write = (changes) => {
    const next = { ...g, ...changes };
    app.editor.setLpWidget(slot, next.kind ? next : null);
  };
  const off = !g.kind;
  return el('div', { class: classes('lp-widget', off && 'off') },
    el('span', { class: 'lp-slot' }, `${col + 1}`),
    Select({ options: LP_WIDGET_KINDS, value: g.kind, 'aria-label': `column ${col + 1}: what it is`,
             onChange: (kind) => write({ kind }) }),
    NumberField({ value: g.cc, min: 0, max: 112, fallback: 20, 'aria-label': `column ${col + 1}: the CC`,
                  disabled: off ? 'true' : null, onChange: (cc) => write({ cc }) }),
    Select({ options: range(17, (c) => `ch ${c}`, 1), value: g.channel || 1, disabled: off ? 'true' : null,
             'aria-label': `column ${col + 1}: the channel`, onChange: (channel) => write({ channel }) }),
    Select({ options: LP_COLOURS, value: g.colour, disabled: off ? 'true' : null,
             'aria-label': `column ${col + 1}: the colour`, style: `border-left: 4px solid ${paletteColour(g.colour || 45)}`,
             onChange: (colour) => write({ colour }) }));
}
