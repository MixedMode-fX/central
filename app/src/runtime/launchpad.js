// The Launchpad on the other end of the surface's cable: drawn in the page,
// and a real one plugged into this computer when there is one.
//
// The module drives a Launchpad in programmer mode (src/surface/launchpad.h):
// it asks the cable who is there, puts what answers into programmer mode,
// reads the pads back as coordinates and lights the LEDs itself. This is the
// *device's* half of that conversation, in the page: it answers the inquiry
// with the family of the model it stands in for, keeps the LEDs the module
// has lit, and turns a press on screen into the note or CC a pad would send.
// Nothing here knows what a page is, what the key is or what a fader does -
// that is the firmware's, which is the point: the picture on this tab is what
// the module put on the wire, byte for byte.
//
// **A real Launchpad can take the virtual one's place.** Bridged, a Launchpad
// plugged into this computer is on the module's cable: its messages go in as
// the virtual one's would, what the module sends to the cable goes out to it,
// and it answers the inquiry itself. The picture on screen is then a mirror
// of the hardware, fed by the same bytes.

import * as P from '../protocol/generated.js';
import { access } from './webmidi.js';

const NOTE_ON = 0x90, NOTE_OFF = 0x80, CONTROL_CHANGE = 0xb0;
const SYSEX_START = 0xf0, REALTIME_FIRST = 0xf8;

// The three MK3 models, as their programmer's references give them: the
// device byte in every message, the family in the answer to an inquiry - the
// same for a Mini and a Pro, which is why the firmware tells them apart by
// the device byte (src/surface/launchpad.h) - and the buttons each has
// around the grid.
export const MODELS = {
  mini: { label: 'Launchpad Mini MK3', device: 0x0d, family: 0x13, firmwareModel: P.LaunchpadModel.LP_MODEL_MINI_MK3,
          left: false, bottom: false },
  x: { label: 'Launchpad X', device: 0x0c, family: 0x03, firmwareModel: P.LaunchpadModel.LP_MODEL_X,
       left: false, bottom: false },
  pro: { label: 'Launchpad Pro MK3', device: 0x0e, family: 0x13, firmwareModel: P.LaunchpadModel.LP_MODEL_PRO_MK3,
         left: true, bottom: true },
};

// The Launchpad's palette, by index, as the programmer's reference draws it.
// The first sixty-four are the module's whole vocabulary (its pages paint with
// a hue and that hue's dark one, in fours from 4 up); the rest are here so a
// byte the module never sends still has a colour rather than a hole.
export const PALETTE = [
  '#000000', '#1e1e1e', '#7f7f7f', '#ffffff', '#ff4c4c', '#ff0000', '#590000', '#190000',
  '#ffbd6c', '#ff5400', '#591d00', '#271b00', '#ffff4c', '#ffff00', '#595900', '#191900',
  '#88ff4c', '#54ff00', '#1d5900', '#142b00', '#4cff4c', '#00ff00', '#005900', '#001900',
  '#4cff5e', '#00ff19', '#00590d', '#001902', '#4cff88', '#00ff55', '#00591d', '#001f12',
  '#4cffb7', '#00ff99', '#005935', '#001912', '#4cc3ff', '#00a9ff', '#004152', '#001019',
  '#4c88ff', '#0055ff', '#001d59', '#000819', '#4c4cff', '#0000ff', '#000059', '#000019',
  '#874cff', '#5400ff', '#190064', '#0f0030', '#ff4cff', '#ff00ff', '#590059', '#190019',
  '#ff4c87', '#ff0054', '#59001d', '#220013', '#ff1500', '#993500', '#795100', '#436400',
  '#033900', '#005932', '#00537f', '#0000ff', '#00454f', '#2500cc', '#7f7f7f', '#202020',
  '#ff0000', '#bdff2d', '#afed06', '#64ff09', '#108b00', '#00ff87', '#00a9ff', '#002aff',
  '#3f00ff', '#7a00ff', '#b21a7d', '#402100', '#ff4a00', '#88e106', '#72ff15', '#00ff00',
  '#3bff26', '#59ff71', '#38ffcc', '#5b8aff', '#3151c6', '#877fe9', '#d31dff', '#ff005d',
  '#ff7f00', '#b9b000', '#90ff00', '#835d07', '#392b00', '#144c10', '#0d5038', '#15152a',
  '#16205a', '#693c1c', '#a8000a', '#de513d', '#d86a1c', '#ffe126', '#9ee12f', '#67b50f',
  '#1e1e30', '#dcff6b', '#80ffbd', '#9a99ff', '#8e66ff', '#404040', '#757575', '#e0ffff',
  '#a00000', '#350000', '#1ad000', '#074200', '#b9b000', '#3f3100', '#b35f00', '#4b1502',
];

export const paletteColour = (index) => PALETTE[index & 0x7f] ?? PALETTE[0];

// The LED ids a Launchpad has, by where they are (src/surface/launchpad.h,
// led_id and friends): the grid from the bottom left, the top row, the right
// column, the logo, and the Pro's extra column and row.
export const gridId = (row, col) => (row + 1) * 10 + col + 1;
export const topId = (col) => 91 + col;
export const sideId = (row) => (row + 1) * 10 + 9;
export const leftId = (row) => (row + 1) * 10;
export const bottomId = (col) => 101 + col;
export const LOGO_ID = 99;

// Whether an id is a pad of the grid (a note in programmer mode) rather than
// a button (a CC).
export const isGridId = (id) => id >= 11 && id <= 88 && id % 10 >= 1 && id % 10 <= 8;

// The ports a computer shows for its Launchpads, cut down to the one each
// device takes programmer mode on.
//
// An MK3 is several USB MIDI interfaces: a Mini or an X has a DAW one and a
// MIDI one, a Pro adds a DIN one that is its MIDI jacks. Only the MIDI
// interface is the device itself - programmer mode, the pads as coordinates
// and the inquiry's answer are all on it - so a Pro bridged on its DIN
// interface stays in its own modes and plays nothing back, and the module
// talks to whatever hangs off its jacks. Every interface is named for its
// role ("LPProMK3 MIDI", "LPProMK3 DIN"), so the others are dropped by name.
// Windows wraps all but the first in "MIDIIN2 (...)" and "MIDIOUT2 (...)",
// which is also what pairs an input with its output there: the wrapper is
// read as the same name with its number, and an input is only ever paired
// with the output of the same interface, never just another Launchpad's.
const LAUNCHPAD_PORT = /launchpad|\bLP(?:X|MiniMK3|ProMK3)\b/i;
const OTHER_INTERFACE = /\b(?:DAW|DIN)\b/i;
const portKey = (name = '') => name.replace(/^MIDI(?:IN|OUT)(\d+)\s*\((.*)\)$/i, '$2 #$1').trim();

export function launchpadPorts(inputs, outputs) {
  return inputs
    .filter((input) => LAUNCHPAD_PORT.test(input.name ?? '') && !OTHER_INTERFACE.test(input.name ?? ''))
    .map((input) => ({
      id: input.id, name: input.name,
      output: outputs.find((o) => portKey(o.name) === portKey(input.name)) ?? null,
    }))
    .filter((d) => d.output);
}

export class VirtualLaunchpad {
  constructor(module) {
    this.module = module;
    // The cable the module has its Launchpad on (state.patch.launchpad.port);
    // 0 while the patch has none.
    this.port = 0;
    // Which Launchpad this stands in for. The firmware reads it off the
    // answer to its inquiry, so changing it here is plugging in another.
    this.model = 'mini';
    // What the module has lit, by LED id: a palette index, or an RGB triple
    // where it sent one.
    this.leds = new Uint8Array(128);
    this.rgb = new Map();
    // Whether the module has put the device into programmer mode. A real
    // Launchpad in a factory mode would be playing its own layout; this one
    // shows nothing until it is asked.
    this.programmer = false;
    this.inquiries = 0;
    this.frames = 0;
    // A Launchpad plugged into this computer, standing in for the drawing.
    this.access = null;
    this.hardware = null;
    this.hardwareInput = null;
    this.unlisten = module?.onPortSysex?.((target, bytes) => this.receive(target, bytes)) ?? null;
  }

  // --- the cable -------------------------------------------------------------

  // Called with the patch's choice on every render: a cable change is a new
  // device on a new wire, which knows nothing of the old one's LEDs.
  follow(port) {
    if (port === this.port) return;
    this.port = port;
    this.reset();
  }

  setModel(model) {
    if (!(model in MODELS) || model === this.model) return;
    this.model = model;
    // Another device answers the next inquiry; the module relinks it.
    this.reset();
  }

  reset() {
    this.leds.fill(0);
    this.rgb.clear();
    this.programmer = false;
  }

  get spec() { return MODELS[this.model]; }

  // --- what the module sends ------------------------------------------------------

  receive(target, bytes) {
    if (!this.port || !(target & this.port)) return;
    if (this.hardware) {
      // The real device gets the bytes and answers for itself; the drawing
      // follows the same bytes.
      try { this.hardware.send(Array.from(bytes)); } catch { /* unplugged mid-frame */ }
    }
    if (bytes.length >= 6 && bytes[1] === P.SYSEX_UNIVERSAL_NON_REALTIME
        && bytes[3] === P.SYSEX_GENERAL_INFORMATION && bytes[4] === P.SYSEX_IDENTITY_REQUEST) {
      this.inquiries++;
      if (!this.hardware) this.answer();
      return;
    }
    // A Launchpad hears only what carries its own device byte.
    if (bytes.length < 8 || bytes[1] !== 0x00 || bytes[2] !== 0x20 || bytes[3] !== 0x29 || bytes[4] !== 0x02
        || bytes[5] !== this.spec.device) return;
    const command = bytes[6];
    if (command === 0x00 && bytes.length === 8) {
      if (!this.hardware) this.answerLayout();
      return;
    }
    if (command === 0x0e) {
      const programmer = bytes[7] === 1;
      if (programmer !== this.programmer) this.reset();
      this.programmer = programmer;
      return;
    }
    if (command !== 0x03) return;
    this.frames++;
    const end = bytes[bytes.length - 1] === 0xf7 ? bytes.length - 1 : bytes.length;
    for (let at = 7; at < end;) {
      const kind = bytes[at];
      const id = bytes[at + 1];
      if (kind === 0x00 || kind === 0x02) { this.light(id, bytes[at + 2]); at += 3; }
      else if (kind === 0x01) { this.light(id, bytes[at + 2]); at += 4; }
      else if (kind === 0x03) {
        this.leds[id & 0x7f] = 0;
        this.rgb.set(id & 0x7f, `rgb(${bytes[at + 2] * 2}, ${bytes[at + 3] * 2}, ${bytes[at + 4] * 2})`);
        at += 5;
      } else break;
    }
  }

  light(id, colour) {
    this.leds[id & 0x7f] = colour & 0x7f;
    this.rgb.delete(id & 0x7f);
  }

  // What an LED shows, as a CSS colour.
  colour(id) { return this.rgb.get(id & 0x7f) ?? paletteColour(this.leds[id & 0x7f]); }
  lit(id) { return this.leds[id & 0x7f] !== 0 || this.rgb.has(id & 0x7f); }

  // The device's answer to a device inquiry: Novation, this model's family.
  answer() {
    const reply = Uint8Array.from([
      0xf0, P.SYSEX_UNIVERSAL_NON_REALTIME, 0x00, P.SYSEX_GENERAL_INFORMATION, P.SYSEX_IDENTITY_REPLY,
      0x00, 0x20, 0x29, this.spec.family, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xf7,
    ]);
    this.module.deliverSysex(this.port, reply);
  }

  // The layout readback's answer, which is what says which model this is.
  answerLayout() {
    const { device } = this.spec;
    this.module.deliverSysex(this.port, Uint8Array.from([0xf0, 0x00, 0x20, 0x29, 0x02, device, 0x00, 0x00, 0xf7]));
  }

  // --- what the device sends ------------------------------------------------------

  // A pad or a button pressed on screen, as programmer mode sends it: a
  // note on channel 1 for the grid, a CC of 127 for a button; released, a
  // note-off or a CC of 0.
  press(id, velocity = 100) {
    if (!this.port) return;
    if (isGridId(id)) this.module.deliverMidi(this.port, NOTE_ON, 1, id, velocity);
    else this.module.deliverMidi(this.port, CONTROL_CHANGE, 1, id, 127);
  }

  release(id) {
    if (!this.port) return;
    if (isGridId(id)) this.module.deliverMidi(this.port, NOTE_OFF, 1, id, 0);
    else this.module.deliverMidi(this.port, CONTROL_CHANGE, 1, id, 0);
  }

  // --- a real Launchpad ----------------------------------------------------------------

  async connect() {
    this.access = await access();
    return this.access;
  }

  // The Launchpads plugged into this computer, one port pair each.
  devices() {
    if (!this.access) return [];
    return launchpadPorts([...this.access.inputs.values()], [...this.access.outputs.values()]);
  }

  get bridged() { return this.hardware?.name ?? ''; }

  // Puts a real Launchpad on the module's cable, or takes it off again.
  bridge(inputId) {
    this.unbridge();
    const device = this.devices().find((d) => d.id === inputId);
    if (!device) return;
    const input = this.access.inputs.get(inputId);
    if (!input) return;
    this.hardware = device.output;
    this.hardwareInput = input;
    input.onmidimessage = (event) => this.fromHardware(event.data);
    // A new device on the wire: the module's next inquiry finds it, and the
    // drawing starts from what it is then told.
    this.reset();
  }

  unbridge() {
    if (this.hardwareInput) this.hardwareInput.onmidimessage = null;
    if (this.hardware) {
      // Back to being a Launchpad: live mode, as the module would leave it.
      try { this.hardware.send([0xf0, 0x00, 0x20, 0x29, 0x02, this.spec.device, 0x0e, 0x00, 0xf7]); } catch { /* gone */ }
    }
    this.hardware = null;
    this.hardwareInput = null;
    this.reset();
  }

  // Bytes from the real device, onto the module's cable as the transport
  // would put them: SysEx whole, the rest as channel messages.
  fromHardware(bytes) {
    if (!this.port) return;
    const status = bytes[0];
    if (status === SYSEX_START) { this.module.deliverSysex(this.port, Uint8Array.from(bytes)); return; }
    if (status >= REALTIME_FIRST || status < 0x80) return;
    this.module.deliverMidi(this.port, status & 0xf0, (status & 0x0f) + 1, bytes[1] ?? 0, bytes[2] ?? 0);
  }
}
