// Where a note goes when you play one.
//
// Every musical thing the page can do - a key on the keyboard, a pad, a pot,
// a Program Change launching a preset - builds a real MIDI message and hands
// it here. **A view does not know which machine it is driving.** That is the
// whole point: the same surface plays the module compiled into the page and
// the module on the end of a cable, and the only difference is which of them
// is running.
//
// Two rules the module's own protocol imposes, and this is where the browser
// side keeps them:
//
//   * **Never the control cable.** MIDI_CONTROL_PORT is reserved for the
//     protocol (src/hal/midi_types.h) so a patch transfer never mixes with
//     musical MIDI and a busy note stream cannot starve a dump. On hardware
//     the two are separate browser ports, so a dump in flight and a pad being
//     hit do not collide.
//   * **Which cable it arrives on is part of the patch.** `CcMapping`
//     filters by source port, so a binding learned while the surface claims
//     one cable does not fire when it claims another. The choice is therefore
//     visible rather than a default nobody is told about - the same control
//     `Controller.setPort` gives an external controller.
//
// A message may name its own cable and channel. The keyboard plays on the one
// the play panel is set to; a surface control carries its own, because a pad
// launching a preset and a pad playing a drum are two different inputs of the
// module as far as the patch is concerned (services/surface.js).

import * as P from '../protocol/generated.js';
import { MUSICAL_PORTS } from '../protocol/names.js';

const NOTE_ON = 0x90;
const NOTE_OFF = 0x80;
const CONTROL_CHANGE = 0xb0;
const PROGRAM_CHANGE = 0xc0;
const ALL_NOTES_OFF = 123;

// The USB cables, in the order a host enumerates them. A Teensy built
// USB_MIDI4_SERIAL offers four, and the browser shows each as a port of its
// own; the DIN sockets and the USB host port are the module's own hardware
// and no browser port reaches them.
const USB_CABLES = [
  P.MidiPort.mmMIDI_USB_0, P.MidiPort.mmMIDI_USB_1,
  P.MidiPort.mmMIDI_USB_2, P.MidiPort.mmMIDI_USB_3,
];

// "Teensy MIDI 2" and "Teensy MIDI" are two cables of one device. A trailing
// number is how every host names them apart.
const baseName = (name) => String(name ?? '').replace(/\s*\d+$/, '').trim();

export class Play {
  constructor({ state, session, module = () => null, listener = () => null,
                refresh = () => {}, render }) {
    this.state = state;
    this.session = session;
    this.moduleOf = module;
    this.listenerOf = listener;
    this.refresh = refresh;
    this.render = render;
    // The MIDI access and the port pair the protocol is using, when the app
    // is driving a module on a cable. Set by `app.connect()`, which already
    // holds both - asking for access again would be a second permission
    // prompt for something the user has allowed once.
    this.access = null;
    this.control = null;
    this.deviceName = '';
  }

  get ui() { return this.state.ui.play; }
  get port() { return this.ui.port; }
  get channel() { return this.ui.channel; }

  setPort(mask) { this.ui.port = mask; this.render(); }

  attach({ access, control, name }) {
    this.access = access;
    this.control = control;
    this.deviceName = name ?? control?.name ?? '';
  }

  detach() {
    this.access = null;
    this.control = null;
    this.deviceName = '';
  }

  // Which machine a note played now would reach, and what it would reach it
  // through. A surface that looks like a rack while it is playing a module
  // inside a browser tab is the failure this exists to prevent, so this is
  // never a guess: `kind` is 'module' when the page's own firmware is
  // running, 'device' when a cable is, and 'nothing' when neither can hear.
  machine(port = this.port) {
    const module = this.moduleOf();
    if (this.session.usingModule) {
      return module
        ? { kind: 'module', name: 'the built-in module', reaches: 'this page', ok: true }
        : { kind: 'nothing', name: 'no module', reaches: 'nothing is running', ok: false };
    }
    const output = this.output(port);
    if (!output) {
      return {
        kind: 'device',
        name: this.deviceName || this.session.transportName,
        reaches: this.whyNot(port),
        ok: false,
      };
    }
    return { kind: 'device', name: this.deviceName || output.name, reaches: output.name, ok: true };
  }

  // The browser port that carries the cable the surface claims. Cable order
  // is the enumeration order of the module's own outputs, which is how a host
  // lists a multi-cable device; a cable with no port behind it is said rather
  // than silently dropped.
  output(port = this.port) {
    const cable = USB_CABLES.indexOf(port);
    if (cable < 0) return null;
    const ports = this.ports();
    const chosen = ports[cable] ?? null;
    if (!chosen || chosen === this.control) return null;
    return chosen;
  }

  // The module's outputs: the ones sharing the control port's name up to a
  // trailing cable number. A single-port interface is a group of one.
  ports() {
    if (!this.access) return [];
    const all = [...this.access.outputs.values()];
    if (!this.control) return all;
    const base = baseName(this.control.name);
    const mine = all.filter((o) => baseName(o.name) === base);
    return mine.length ? mine : all;
  }

  whyNot(port = this.port) {
    if (!this.access) return 'the page is not connected to a module';
    const cable = USB_CABLES.indexOf(port);
    const named = MUSICAL_PORTS.find((p) => p.value === port)?.label ?? 'that cable';
    if (cable < 0) return `${named} is one of the module's own sockets: no browser port reaches it`;
    if (this.ports()[cable] === this.control) return `${named} is carrying the protocol`;
    return `this computer has no port for ${named}`;
  }

  // --- playing ---------------------------------------------------------------

  // `where` is anything carrying a `port` and a `channel` - a pad, a pot -
  // and each is taken on its own: a control that names a cable and not a
  // channel plays on the panel's channel.
  cable(where) {
    return { port: where?.port ?? this.port, channel: where?.channel ?? this.channel };
  }

  noteOn(pitch, velocity = this.ui.velocity, where = null) { this.send(NOTE_ON, pitch, velocity, where); }
  noteOff(pitch, where = null) { this.send(NOTE_OFF, pitch, 0, where); }
  cc(number, value, where = null) { this.send(CONTROL_CHANGE, number, value, where); }

  // A Program Change recalls a preset on the module, quantised to wherever
  // `pc_quantise` says (src/protocol/sysex_handler.h). It is one data byte,
  // not two.
  programChange(program, where = null) { this.send(PROGRAM_CHANGE, program, 0, where); }

  allNotesOff() {
    this.cc(ALL_NOTES_OFF, 0);
    this.listenerOf()?.allOff();
  }

  // One message, to whichever machine is live. Returns what the module made
  // of it when that can be known - how many of its ports took the event, or
  // null when the control plane consumed it - and null on a cable, where the
  // only honest answer is that nobody said.
  send(type, d1 = 0, d2 = 0, where = null) {
    const { port, channel } = this.cable(where);
    const module = this.moduleOf();
    if (this.session.usingModule) {
      if (!module) return null;
      const accepted = module.deliverMidi(port, type, channel, d1, d2);
      // The lights, the roll and the meters are painted off the module's own
      // frames; a note played between two of them has to show at once.
      this.refresh();
      return accepted;
    }
    const output = this.output(port);
    if (!output) return null;
    const status = (type & 0xf0) | ((channel - 1) & 0x0f);
    try {
      output.send(type === PROGRAM_CHANGE ? [status, d1 & 0x7f] : [status, d1 & 0x7f, d2 & 0x7f]);
    } catch {
      // A port that went away between the gesture and the send. The surface
      // says which machine is live on every render, so this needs no notice
      // of its own.
    }
    return null;
  }
}
