// An external MIDI controller, playing the module in this page.
//
// The module the app is editing is usually the one embedded in the page, and a
// patch is a thing you play, not a thing you read. So the keys, pads and knobs
// on a controller plugged into the *computer* are routed into the module's
// MIDI input. The other direction - what the module plays, out of this
// computer and into a synth - is `runtime/midiout.js`.
//
// Two things follow from routing a controller in rather than around:
//
//   * **learn works with no module in the room.** An incoming CC takes the
//     path main.cpp gives it - preset recall, then NRPN, then the binding
//     table, then the graph - because `EmbeddedModule.deliverMidi()` offers it
//     in that order to the firmware's own control plane. Arm learn beside a
//     parameter, turn a knob on the desk, and the binding is made by the
//     firmware, not by the page.
//   * **the patch is played by the thing it was written for.** A keyboard split,
//     a channel filter or a source-port rule can be tried with the controller
//     that will be plugged into the module, before there is a module.
//
// Web MIDI is not everywhere - no Safari, none on iOS - which is a browser
// limit, not a limit of the app: everything else here works without it.

import * as P from '../protocol/generated.js';
import { MUSICAL_PORTS } from '../protocol/names.js';
import { access } from './webmidi.js';

const SYSEX_START = 0xf0, REALTIME_FIRST = 0xf8;

export class Controller {
  constructor(module) {
    this.module = module;
    this.access = null;
    this.inputId = '';
    // Which of the module's MIDI inputs the controller appears on. A patch
    // filters by source port, so this is part of what is being tested: the
    // default is the first musical port, never the control cable.
    this.port = MUSICAL_PORTS[0]?.value ?? P.MidiPort.mmMIDI_USB_0;
  }

  get inputs() { return this.access ? [...this.access.inputs.values()] : []; }

  get input() { return this.access?.inputs.get(this.inputId) ?? null; }

  async connect() {
    this.access = await access();
    return this.access;
  }

  // Only one input is listened to at a time. Two would be ambiguous the moment
  // a binding is learned - "which knob was that?" - and a merge belongs in the
  // MIDI interface, not here.
  listenTo(id) {
    for (const input of this.inputs) input.onmidimessage = null;
    this.inputId = id;
    const input = this.input;
    if (input) input.onmidimessage = (event) => this.receive(event.data, event.timeStamp);
  }

  setPort(mask) { this.port = mask; }

  // `atMs` is the event's own `timeStamp`: when the bytes reached the
  // browser, not when this handler got to run. The module stamps the message
  // with it, which is what a clock measured between two bytes depends on.
  receive(bytes, atMs = undefined) {
    const status = bytes[0];
    if (status === SYSEX_START) return;             // a controller's SysEx is not the module's business
    if (status >= REALTIME_FIRST) {
      // Clock, start, stop and continue: the module's clock reads these when
      // its source is MIDI, so an external tempo can drive the patch.
      this.module.deliverRealtime(this.port, status, atMs);
      return;
    }
    if (status < 0x80) return;
    const type = status & 0xf0;
    const channel = (status & 0x0f) + 1;            // the firmware counts channels from 1
    this.module.deliverMidi(this.port, type, channel, bytes[1] ?? 0, bytes[2] ?? 0, atMs);
  }
}
