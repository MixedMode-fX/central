// What a control does, asked for by turning **edit** on in the bar and
// pressing it.
//
// One sheet over the surface rather than a second page: the surface is the
// stage view, and a control that had to be configured somewhere else is a
// control nobody would ever reassign mid-set. Everything here is either the
// performer's - the caption, the colour, the number it sends - or one gesture
// away from the patch's: "bind it" arms the module's own learn, and the
// binding is made by the firmware when the control moves.
//
// **Windows and depths are not here.** A macro destination carries two
// numbers you cannot gesture, and typing numbers into a stage view is what
// stops it being one. That is the bench's job (the macros panel).
//
// The sheet is a panel like the surface behind it: the caption first and
// biggest, because it is the silkscreen and what will be read at arm's
// length; the labels in the panel face; the fields in pairs rather than a
// column of stacked labels; and numbers as steppers, because a native
// spinner's arrows are three pixels wide under a thumb.
//
// **The cable is not here.** Which of the module's inputs a control arrives on
// decides what hears it - a MIDI input takes a port mask, a binding filters by
// source port, and so does Program Change recall - and it is one lead for the
// whole surface, chosen in the bar. This sheet says which lead that is,
// because a channel means nothing without it.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Field, Fields } from '../components/Field.js';
import { Row, Hint } from '../components/Panel.js';
import { Segmented } from '../components/Segmented.js';
import { Select, range } from '../components/Select.js';
import { openMenu, MenuItem } from '../components/Menu.js';
import { PadKind, PadMode, COLOURS } from '../../services/surface.js';
import { SWAP_TIMINGS, channelLabel, portNames } from '../../protocol/names.js';
import { knobParams } from '../../core/patch.js';
import { describeTarget } from '../panels/ModMatrix.js';
import { noteName } from '../../core/music.js';

const KINDS = [
  { value: PadKind.NOTE, label: 'note', hint: 'a note on while it is held' },
  { value: PadKind.CC, label: 'CC', hint: '127 down, 0 up' },
  { value: PadKind.PROGRAM, label: 'launch', hint: 'a Program Change: recall a stored patch' },
];

const MODES = [
  { value: PadMode.MOMENTARY, label: 'momentary', hint: 'on while it is held' },
  { value: PadMode.TOGGLE, label: 'latch', hint: 'alternates, and stays' },
];

export function AssignSheet(app, where) {
  const surface = app.surface;
  const control = surface.control(where);
  const close = () => { app.state.ui.surface.editing = null; app.render(); };
  const set = (changes) => (where.kind === 'pot'
    ? surface.setPot(where.index, changes)
    : surface.setPad(where.index, changes));

  return el('div', { class: 'sheet', role: 'dialog', 'aria-label': 'what this control does' },
    el('div', { class: 'sheet-bar' },
      el('span', { class: 'sheet-which' },
        where.kind === 'pot' ? `pot ${where.index + 1}` : `pad ${where.index + 1}`),
      el('button', { class: 'ghost', onclick: close }, 'done')),
    el('div', { class: 'sheet-body' },
      el('input', {
        type: 'text', class: 'sheet-caption', value: control.label, maxlength: '10',
        placeholder: where.kind === 'pot' ? `cc ${control.cc}` : `pad ${where.index + 1}`,
        'aria-label': 'what this control is called',
        oninput: (e) => { control.label = e.target.value; },
        onchange: () => set({}),
      }),
      Colours(control.colour, (colour) => set({ colour })),
      where.kind === 'pot' ? PotFields(app, where, control, set) : PadFields(app, where, control, set),
      BindingSection(app, where, control)));
}

function Colours(current, onPick) {
  return el('div', { class: 'swatches', role: 'group', 'aria-label': 'colour' },
    COLOURS.map((colour) => el('button', {
      type: 'button', class: classes('swatch', `hue-${colour}`, colour === current && 'on'),
      'aria-label': colour, 'aria-pressed': colour === current ? 'true' : 'false',
      onclick: () => onPick(colour),
    })));
}

// A real channel, 1..16: a surface control sends on one cable and one
// channel, so omni - which is a *listening* rule - is not one of the answers.
const ChannelField = (value, onChange) => Select({
  options: range(17, channelLabel, 1), value, onChange, 'aria-label': 'channel',
});

// A number, as two big targets either side of a readout. Everything set here
// is set with a thumb, over a surface that is being played with one.
function Stepper({ value, min, max, label, onChange }) {
  const at = Math.max(min, Math.min(max, value));
  const step = (by) => el('button', {
    type: 'button', 'aria-label': `${label} ${by > 0 ? 'up' : 'down'}`,
    disabled: (by > 0 ? at >= max : at <= min) ? 'disabled' : null,
    onclick: () => onChange(Math.max(min, Math.min(max, at + by))),
  }, by > 0 ? '+' : '−');
  return el('div', { class: 'stepper', role: 'group', 'aria-label': label },
    step(-1),
    el('span', { class: 'stepper-value' }, String(at)),
    step(1));
}

// The lead this control is on, which is the surface's and not its own, and
// whether anything is on the other end of it: a cable no browser port reaches
// is a cable nothing hears, and the channel below would be the only thing on
// screen if that went unsaid.
function cableHint(app) {
  const port = app.surface.port;
  const machine = app.play.machine(port);
  const named = portNames(port).join(', ') || 'no cable';
  return Hint(`the whole surface sends on ${named}`
    + (machine.ok ? '' : ` — ${machine.reaches}`));
}

function PotFields(app, where, pot, set) {
  return el('div', { class: 'stack' },
    Fields(
      Field({ label: 'sends cc' }, Stepper({
        value: pot.cc, min: 0, max: 119, label: 'CC number', onChange: (cc) => set({ cc }),
      })),
      Field({ label: 'channel' }, ChannelField(pot.channel, (channel) => set({ channel })))),
    cableHint(app));
}

function PadFields(app, where, pad, set) {
  const kind = el('div', { class: 'fields one' },
    Field({ label: 'sends' }, Segmented({
      options: KINDS, value: pad.kind, label: 'what this pad sends',
      onChange: (k) => set({ kind: k }),
    })));
  const channel = Field({ label: 'channel' },
    ChannelField(pad.channel, (c) => set({ channel: c })));
  if (pad.kind === PadKind.PROGRAM) {
    return el('div', { class: 'stack' },
      kind, el('div', { class: 'fields one' }, channel), cableHint(app), Launch(app, pad, set));
  }

  const what = [];
  if (pad.kind === PadKind.NOTE) {
    what.push(Field({ label: `note · ${noteName(pad.note)}` }, Stepper({
      value: pad.note, min: 0, max: 127, label: 'note number', onChange: (note) => set({ note }),
    })));
    what.push(Field({ label: 'velocity' }, Stepper({
      value: pad.velocity, min: 1, max: 127, label: 'velocity',
      onChange: (velocity) => set({ velocity }),
    })));
    what.push(channel);
  } else {
    what.push(Field({ label: 'sends cc' }, Stepper({
      value: pad.cc, min: 0, max: 119, label: 'CC number', onChange: (cc) => set({ cc }),
    })));
    what.push(channel);
  }
  return el('div', { class: 'stack' },
    kind,
    Fields(...what),
    el('div', { class: 'fields one' },
      // The caveat only where it applies: a momentary pad claims nothing
      // about the module, so it needs no line about what it is not saying.
      Field({ label: 'press',
              hint: pad.mode === PadMode.TOGGLE
                ? 'a latch shows what it last sent, not what the module holds' : null },
        Segmented({ options: MODES, value: pad.mode, label: 'how this pad behaves',
                    onChange: (mode) => set({ mode }) }))),
    cableHint(app));
}

// A launch pad recalls one of the module's stored patches. Where that lands
// is `GlobalSettings::pc_quantise`, which the firmware has implemented since
// the protocol was written and nothing on screen could reach until now: a
// recall mid-bar glitches, one that waits for the bar does not.
function Launch(app, pad, set) {
  const g = app.state.globals;
  const slots = app.device?.capabilities?.slots ?? P.PATCH_SLOTS;
  return el('div', { class: 'stack' },
    Fields(
      // Slot 0 is the autosaved patch on screen, so it is not a scene to
      // launch: the launchable ones are the rest.
      Field({ label: 'recalls' }, Select({
        options: range(slots, (s) => `preset ${s}`, 1), value: pad.program,
        'aria-label': 'which stored patch this pad launches',
        onChange: (program) => set({ program }),
      })),
      Field({ label: 'lands' }, Segmented({
        options: SWAP_TIMINGS, value: g.pcQuantise ?? 0, label: 'when a recall lands',
        onChange: (pcQuantise) => app.editor.setGlobals({ pcQuantise }, 'recall timing'),
      }))),
    g.pcEnabled
      ? Hint(recallSaid(g, pad, app.surface.port))
      : Row(el('span', { class: 'hint' }, 'the module is not recalling Program Change'),
            el('button', {
              onclick: () => app.editor.setGlobals(
                { pcEnabled: 1, pcChannel: pad.channel, pcSourceMask: app.surface.port },
                'Program Change recall'),
            }, 'turn recall on')));
}

// Whether this pad's Program Change is one the module is listening for. The
// channel was always half the answer; the cable is the other half, and a
// recall filtered to a port this pad does not send on is silence with no
// explanation anywhere on screen (src/protocol/sysex_handler.h).
function recallSaid(g, pad, port) {
  const where = g.pcChannel ? `channel ${g.pcChannel}` : 'any channel';
  const ports = g.pcSourceMask ? portNames(g.pcSourceMask).join(', ') : 'any cable';
  const deaf = (g.pcChannel && g.pcChannel !== pad.channel)
    || (g.pcSourceMask && (g.pcSourceMask & port) === 0);
  return `the module recalls Program Change on ${where}, from ${ports}`
    + (deaf ? ' — not what this pad sends' : '');
}

// --- what it moves ------------------------------------------------------------

function BindingSection(app, where, control) {
  const surface = app.surface;
  const bound = surface.bindingOf(control);
  const armed = surface.armedOn(where);
  const b = surface.budget();
  const pad = where.kind === 'pad';
  // A note pad is played by the graph, not by the binding table: a binding
  // reads CC numbers, so there is nothing here for a pad that sends notes.
  if (pad && surface.pad(where.index).kind !== PadKind.CC) {
    return Hint(surface.pad(where.index).kind === PadKind.PROGRAM
      ? 'a launch pad is answered by the module itself, not by a binding'
      : 'a note goes into the patch, on the surface’s cable');
  }

  return el('div', { class: 'sheet-binds' },
    el('h4', {}, 'moves'),
    el('p', { class: classes('bind-what', !bound && 'hint') },
      bound ? describeTarget(app, bound.mapping) : 'nothing yet'),
    armed
      ? Row(el('span', { class: 'hint' }, where.kind === 'pot' ? 'turn it to bind it' : 'press it to bind it'),
            el('button', { class: 'ghost', onclick: () => app.surface.disarm() }, 'cancel'))
      : Row(el('button', {
              onclick: (e) => openTargets(app, e.currentTarget.getBoundingClientRect(), where, control),
            }, bound ? 'bind it to something else' : 'bind it'),
            bound ? el('button', { class: 'ghost danger', onclick: () => surface.clearBinding(control) }, 'clear') : null),
    Hint(`${b.bindings}/${b.bindingSlots} bindings in this patch`
       + (b.free ? '' : ' — the table is full')));
}

// The targets, macros first: a macro is what a performance control is *for*,
// and one control moving four parameters is the reason the macro table
// exists at all. Picking one arms the module's learn - the binding is made by
// the firmware when the control moves, so takeover, the relative encodings
// and 14-bit pairing stay implemented exactly once.
function openTargets(app, at, where, control) {
  const surface = app.surface;
  const bound = surface.bindingOf(control);
  const onMacro = bound?.mapping.targetKind === P.CcTargetKind.CC_TARGET_MACRO
    ? bound.mapping.targetIndex : null;
  const items = [];

  for (const { index, macro } of surface.macros()) {
    items.push(MenuItem({
      label: `macro ${index + 1} · ${macro.name}`,
      hint: index === onMacro ? 'already on this control' : 'one control, several parameters',
      class: index === onMacro ? 'chosen' : '',
      onPick: () => surface.arm(where, {
        targetKind: P.CcTargetKind.CC_TARGET_MACRO, targetIndex: index, param: 0,
      }),
    }));
  }

  for (const [index, node] of (app.state.patch.nodes ?? []).entries()) {
    const d = app.device?.byId.get(node.algorithmId);
    for (const { at: param, pd } of knobParams(d)) {
      items.push(MenuItem({
        label: `${d?.name ?? node.algorithmId} ${index} · ${pd.name}`,
        // On a control that already drives a macro, a parameter joins *that
        // macro* rather than replacing what the control does: full travel and
        // half the parameter's range, which is a destination you can hear
        // before you go and shape it at the bench.
        hint: onMacro === null ? `${pd.min}–${pd.max}` : `add to macro ${onMacro + 1}`,
        onPick: () => (onMacro === null
          ? surface.arm(where, {
              targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: index, param,
            })
          : addToMacro(app, onMacro, index, param, pd)),
      }));
    }
  }

  if (!items.length) items.push(MenuItem({ label: 'nothing to bind to', hint: 'the patch is empty', onPick: () => {} }));

  openMenu({
    at, kind: 'cc',
    head: onMacro === null ? 'what does this control move?' : `adding to macro ${onMacro + 1}`,
    items,
  });
}

function addToMacro(app, macro, targetIndex, param, pd) {
  const depth = Math.max(1, Math.round(((pd?.max ?? 255) - (pd?.min ?? 0)) / 2));
  const slot = app.editor.addMacroDest({
    macro, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex, param,
    srcLo: 0, srcHi: 255, depth, flags: 0,
  });
  if (slot !== null) app.editor.say(`${pd?.name ?? `parameter ${param}`} follows macro ${macro + 1}`);
}
