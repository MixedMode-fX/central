// The performance surface: sixteen pads, eight pots, and what each of them
// does.
//
// **The surface belongs to the performer; the bindings belong to the patch.**
// A pad's caption, its colour, its CC number, whether it latches and the one
// cable the whole surface is plugged into are *your controller* - they live in
// this browser, beside the canvas arrangement, and they are keyed globally
// rather than per patch, because a pad that changed meaning with every patch
// load would be a pad nobody could learn. What the CC reaches is the patch's
// business and travels in the patch image.
//
// **Fixed geometry, soft assignment.** You cannot move a control; you can
// change everything about what it does. That is what makes a surface a
// surface rather than a layout editor, and it is what the module's eventual
// front panel will be: a fixed number of holes in a piece of aluminium.
//
// **The surface is a dumb MIDI controller.** It emits CC, notes and Program
// Change. A Launchpad sending the same CC is indistinguishable from it, which
// is why the mapping layer stays in the firmware (src/control/cc_mapper.h)
// and nothing here reaches into a patch to move a parameter.

import * as P from '../protocol/generated.js';
import { ALL_MUSICAL } from '../protocol/names.js';
import { isBinding, isMacro, freeCcSlot } from '../core/patch.js';

export const POTS = 8;
export const PADS = 16;
export const PAD_COLUMNS = 4;

// CC 20..27 are undefined by the MIDI specification, so a factory surface
// steps on nothing. Pads start at note 36, the bottom left of every pad grid
// since the MPC.
const FIRST_POT_CC = 20;
const FIRST_PAD_NOTE = 36;

// The cable the surface is plugged into. **One for the whole surface**, the
// way a controller has one lead: a MIDI input takes a port mask and
// `CcMapping` filters by source port, so the cable decides what in the patch
// can hear this thing at all - and a box of sixteen pads each on a different
// lead is not a controller anybody could reason about. The on-screen keyboard
// is a separate instrument and keeps its own (state.ui.play). The first USB
// cable is the factory answer because it is the one a browser can always
// reach.
const FIRST_PORT = P.MidiPort.mmMIDI_USB_0;

export const PadKind = { NOTE: 'note', CC: 'cc', PROGRAM: 'program' };
export const PadMode = { MOMENTARY: 'momentary', TOGGLE: 'toggle' };

// The domain colours, reused: a surface where every control is a different
// colour says nothing, so these are the five the rest of the app already
// paints with.
export const COLOURS = ['accent', 'gate', 'note', 'cv', 'warn'];

const clamp7 = (v) => Math.max(0, Math.min(127, Math.round(v)));

const emptyPot = (i) => ({
  cc: FIRST_POT_CC + i, channel: 1, label: '', colour: COLOURS[0], value: 0,
});

const emptyPad = (i) => ({
  kind: PadKind.NOTE,
  note: FIRST_PAD_NOTE + i,
  cc: 40 + i,
  program: 1,
  velocity: 100,
  channel: 1,
  mode: PadMode.MOMENTARY,
  label: '',
  colour: COLOURS[i % COLOURS.length],
  on: false,
});

// A stored document is read strictly: anything missing is the factory value
// for that control rather than an optional field the rest of the app has to
// keep testing for.
function normalise(stored) {
  const doc = { port: stored?.port ?? FIRST_PORT, pots: [], pads: [] };
  for (let i = 0; i < POTS; i++) doc.pots.push({ ...emptyPot(i), ...(stored?.pots?.[i] ?? {}) });
  for (let i = 0; i < PADS; i++) doc.pads.push({ ...emptyPad(i), ...(stored?.pads?.[i] ?? {}) });
  return doc;
}

export class Surface {
  constructor({ state, library, play, editor, session, render }) {
    this.state = state;
    this.library = library;
    this.play = play;
    this.editor = editor;
    this.session = session;
    this.render = render;
    this.doc = normalise(library.readSurface());
  }

  save() { this.library.writeSurface(this.doc); }

  // The one cable, and what a control sends on: the surface's lead, and the
  // control's own channel.
  get port() { return this.doc.port; }

  setPort(port) {
    this.doc.port = port;
    this.save();
    this.render();
  }

  wire(control) { return { port: this.doc.port, channel: control.channel }; }

  pot(index) { return this.doc.pots[index]; }
  pad(index) { return this.doc.pads[index]; }
  control(where) { return where.kind === 'pot' ? this.pot(where.index) : this.pad(where.index); }

  setPot(index, changes) {
    Object.assign(this.doc.pots[index], changes);
    this.save();
    this.render();
  }

  setPad(index, changes) {
    Object.assign(this.doc.pads[index], changes);
    this.save();
    this.render();
  }

  reset() {
    this.doc = normalise(null);
    this.save();
    this.render();
  }

  // --- playing ---------------------------------------------------------------

  // **Nothing here re-renders the page.** A control is played with a finger on
  // it, and rebuilding the tree under that finger drops the gesture and
  // flashes every other control on the surface. So a move or a press writes
  // the state, sends the MIDI and returns what the display should say; the
  // view writes that and the one class that changed straight into the DOM.
  //
  // A pot moved. The document is written once, when the gesture ends: a sweep
  // is a hundred values and localStorage is not a fader.
  turn(index, value, { commit = false } = {}) {
    const pot = this.pot(index);
    pot.value = clamp7(value);
    this.play.cc(pot.cc, pot.value, this.wire(pot));
    if (commit) this.save();
    return pot.value;
  }

  // Returns the reading for the display: what this press did, which for a
  // launch pad is the part that is not the pad's own business.
  press(index) {
    const pad = this.pad(index);
    if (pad.kind === PadKind.PROGRAM) {
      this.play.programChange(pad.program, this.wire(pad));
      return this.launchSaid(pad);
    }
    if (pad.mode === PadMode.TOGGLE) {
      pad.on = !pad.on;
      this.emit(pad, pad.on);
      this.save();
      return pad.on ? 'on' : 'off';
    }
    pad.on = true;
    this.emit(pad, true);
    return pad.kind === PadKind.NOTE ? String(pad.velocity) : '127';
  }

  release(index) {
    const pad = this.pad(index);
    if (pad.kind === PadKind.PROGRAM || pad.mode === PadMode.TOGGLE) return;
    if (!pad.on) return;
    pad.on = false;
    this.emit(pad, false);
  }

  emit(pad, on) {
    if (pad.kind === PadKind.NOTE) {
      if (on) this.play.noteOn(pad.note, pad.velocity, this.wire(pad));
      else this.play.noteOff(pad.note, this.wire(pad));
      return;
    }
    this.play.cc(pad.cc, on ? 127 : 0, this.wire(pad));
  }

  // What hitting a launch pad does, in words, including the part that is not
  // this pad's to decide: recall has to be enabled and the quantise is one
  // setting for the whole patch.
  launchSaid(pad) {
    const g = this.state.globals;
    if (!g.pcEnabled) return 'not recalling';
    return ['now', 'next beat', 'next bar'][g.pcQuantise] ?? 'now';
  }

  // --- what a control reaches -------------------------------------------------

  // The binding this control's CC would move, if any. Matched the way the
  // firmware matches it (src/control/cc_mapper.cpp): the cable, the number,
  // and a channel that is this one or omni. The cable is in it because the
  // module's own learn writes the single port the CC arrived on, so moving
  // the surface's lead leaves every binding learned on the old one behind -
  // and a sheet that said otherwise would be describing a control nobody
  // could hear.
  bindingOf(control) {
    const map = this.state.patch.ccMap ?? [];
    const slot = map.findIndex((m) => isBinding(m) && m.cc === control.cc
      && (m.sourceMask & this.doc.port) !== 0
      && (m.channel === 0 || m.channel === control.channel));
    return slot < 0 ? null : { slot, mapping: map[slot] };
  }

  // Everything a control can be pointed at: the patch's macros first, because
  // a macro is the thing a performance control is *for*, then every parameter
  // a knob can reach.
  macros() {
    const table = this.state.patch.macros ?? [];
    return table.map((m, index) => ({ index, macro: m }))
      .filter(({ macro }) => isMacro(macro));
  }

  // What the budget looks like before "bind" can fail. `N_CC_MAP` is shared
  // by every binding in the patch, and a macro spends one of them to be
  // driven at all, so the spend is worth showing rather than arriving as a
  // NAK on a control somebody is mid-gesture with.
  budget() {
    const caps = this.editor.caps;
    const map = this.state.patch.ccMap ?? [];
    const pool = this.state.patch.macroDest ?? [];
    return {
      bindings: map.filter(isBinding).length,
      bindingSlots: caps?.ccMappings ?? P.N_CC_MAP,
      macros: this.macros().length,
      macroSlots: caps?.macros ?? P.N_MACRO,
      dests: pool.filter((d) => d && d.macro !== null && d.macro !== undefined && d.macro !== 0xff).length,
      destSlots: caps?.macroDests ?? P.N_MACRO_DEST,
      free: freeCcSlot(this.state.patch, caps?.ccMappings) !== null,
    };
  }

  // Arm the module to bind the next CC it sees to `target`, and say so. The
  // gesture is finished by *moving the control*, which is the same reason
  // opening the learn menu beside a parameter is itself the arm: the module's
  // own learn is what makes the binding, so takeover, the relative encodings
  // and 14-bit pairing are implemented once, in the firmware, and a surface
  // control binds exactly as a knob on the desk does.
  arm(where, target) {
    this.state.ui.surface.armed = where;
    this.editor.learnInto(target);
    this.state.status = where.kind === 'pot'
      ? 'turn the pot to bind it'
      : 'press the pad to bind it';
    this.render();
  }

  disarm() {
    this.state.ui.surface.armed = null;
    this.editor.cancelLearn();
  }

  armedOn(where) {
    const armed = this.state.ui.surface.armed;
    return Boolean(this.editor.learnTarget && armed
      && armed.kind === where.kind && armed.index === where.index);
  }

  // A plain binding, written rather than learned: the same record the module's
  // own learn would have made, for the one case where there is nothing to
  // move - a pad that sends Program Change, or a page being set up with no
  // module listening.
  bind(control, target) {
    const existing = this.bindingOf(control);
    const slot = existing ? existing.slot : freeCcSlot(this.state.patch, this.editor.caps?.ccMappings);
    if (slot === null) {
      this.editor.fail(`every one of the module's ${this.budget().bindingSlots} bindings is in use`);
      return;
    }
    this.editor.setCcMap(slot, {
      sourceMask: ALL_MUSICAL,
      channel: 0,
      cc: control.cc,
      targetKind: target.targetKind,
      targetIndex: target.targetIndex,
      param: target.param,
      // The target's own full range, which the firmware reads from the
      // parameter descriptors, and a knob that jumps: a control that had to
      // be set up before it moved anything is a control nobody made.
      min: 0,
      max: 0,
      flags: P.CcFlags.CC_TAKEOVER_JUMP,
    });
  }

  clearBinding(control) {
    const existing = this.bindingOf(control);
    if (!existing) return;
    this.editor.clearMapping(existing.slot);
  }
}
