// MIDI: what comes in, what goes out, what the clock follows, and the
// controller plugged into this computer. What a controller *moves* is not
// here: a binding is part of the patch, so it lives in the mod matrix. What
// is left is the room - the cables, the channels, the clock and Program
// Change recall, none of which a patch travels with.

import { el } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { Select } from '../components/Select.js';
import { NumberField } from '../components/NumberField.js';
import { Switch } from '../components/Switch.js';
import { IconButton } from '../components/IconButton.js';
import { ChannelSelect } from '../controls/ChannelSelect.js';
import { PortToggles } from '../controls/PortToggles.js';
import { ClockFields } from '../controls/ClockFields.js';
import { RouteCard } from '../panels/RouteCard.js';
import { MUSICAL_PORTS, SWAP_TIMINGS } from '../../protocol/names.js';
import { describeSupport } from '../../runtime/webmidi.js';
import '../panels/cards.css';

export function MidiTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  return el('div', {}, ControllerPanel(app), RoutingPanel(app), GlobalsPanel(app));
}

// --- routing ---------------------------------------------------------------

// The ports a patch is using, and a way to take one more into use. An unused
// port is not drawn: eight cards, six of them empty, was most of a phone
// screen spent saying "no".
function RouteSide(app, isOut) {
  const caps = app.device.capabilities;
  const slots = (isOut ? app.state.patch.midiOut : app.state.patch.midiIn).slice(0, isOut ? caps.midiOut : caps.midiIn);
  const used = slots.map((port, index) => ({ port, index }))
    .filter(({ port }) => (isOut ? port.targetMask : port.sourceMask));

  return el('div', { class: 'route-side' },
    el('div', { class: 'route-side-head' },
      el('h3', {}, isOut ? 'outputs' : 'inputs'),
      el('span', { class: 'hint' }, `${used.length}/${slots.length}`),
      used.length < slots.length
        ? IconButton({ icon: 'plus', class: 'ghost', label: `add a MIDI ${isOut ? 'output' : 'input'}`,
                       onclick: () => app.editor.addMidiPort(isOut) })
        : null),
    used.length
      ? used.map(({ index }) => RouteCard(app, index, isOut))
      : Hint(isOut ? 'nothing is played out' : 'nothing is taken in'));
}

export function RoutingPanel(app) {
  return Panel('MIDI routing', el('div', { class: 'routes' }, RouteSide(app, false), RouteSide(app, true)));
}

// --- clock, Program Change, NRPN -------------------------------------------

export function GlobalsPanel(app) {
  const g = app.state.globals;
  const set = (changes) => app.editor.setGlobals(changes);
  const nrpn = (changes) => app.editor.setNrpn(changes);

  return Panel('clock and recall',
    Fields(
      ...ClockFields(app),
      Field({ label: 'CV pulses per quarter' }, NumberField({
        value: g.cvPpqn, min: 1, max: 96, fallback: 4, wide: true, 'aria-label': 'CV pulses per quarter note',
        onChange: (cvPpqn) => set({ cvPpqn }),
      })),
      Field({ label: 'Program Change recalls presets' }, Switch({
        checked: g.pcEnabled !== 0, label: g.pcEnabled ? 'on' : 'off',
        onChange: (on) => set({ pcEnabled: on ? 1 : 0 }),
      })),
      Field({ label: 'recall listens on' }, ChannelSelect({ value: g.pcChannel, onChange: (pcChannel) => set({ pcChannel }) })),
      Field({ label: 'recall lands' }, Select({
        options: SWAP_TIMINGS, value: g.pcQuantise, onChange: (pcQuantise) => set({ pcQuantise }),
      }))),
    Fields(
      Field({ label: 'from these ports', hint: 'none = any' }, PortToggles({
        mask: g.pcSourceMask, label: 'Program Change source ports',
        onChange: (pcSourceMask) => set({ pcSourceMask }),
      }))),
    el('h2', { class: 'spaced' }, 'NRPN'),
    Fields(
      Field({ label: 'accept NRPN' }, Switch({
        checked: g.nrpnEnabled !== 0, label: g.nrpnEnabled ? 'on' : 'off',
        onChange: (on) => nrpn({ nrpnEnabled: on ? 1 : 0 }),
      })),
      Field({ label: 'on channel' }, ChannelSelect({ value: g.nrpnChannel, onChange: (nrpnChannel) => nrpn({ nrpnChannel }) }))),
    Fields(
      Field({ label: 'from these ports', hint: 'none = any' }, PortToggles({
        mask: g.nrpnSourceMask, label: 'NRPN source ports',
        onChange: (nrpnSourceMask) => nrpn({ nrpnSourceMask }),
      }))));
}

// --- an external controller ------------------------------------------------

// A controller plugged into the *computer*, playing the module in the page.
// The keys and knobs on the desk reach the patch being edited, on a chosen
// port and its own channel, through the same MIDI input a cable would use -
// so learn works from it, with no module in the room.
export function ControllerPanel(app) {
  const controller = app.controller;
  const support = describeSupport();
  const title = 'external controller';

  if (!app.session.usingModule) return Panel(title, Hint('plug it into the module on the cable'));
  if (!support.ok) return Panel(title, Hint(support.reason));
  if (!controller?.access) {
    return Panel(title, Row(el('button', { onclick: () => app.connectController() }, 'find my MIDI devices')));
  }

  const ports = (list, chosen, onChange) => Select({
    class: 'grow', value: chosen,
    options: [{ value: '', label: 'nothing' }, ...list.map((port) => ({ value: port.id, label: port.name }))],
    onChange: (id) => { onChange(id); app.render(); },
  });
  return Panel(title,
    Fields(
      Field({ label: 'play the module from' }, ports(controller.inputs, controller.inputId, (id) => controller.listenTo(id))),
      Field({ label: 'arriving on' }, Select({
        options: MUSICAL_PORTS, value: controller.port,
        onChange: (mask) => { controller.setPort(mask); app.render(); },
      })),
      Field({ label: 'send what it plays to' }, ports(controller.outputs, controller.outputId, (id) => controller.sendTo(id)))));
}

