// What the module in this page plays, out of this computer's MIDI ports.
//
// The module has eight MIDI cables of its own (`MidiPort` in
// src/hal/midi_types.h) and a MIDI out node names a mask of them; the browser
// has one port per socket on whatever interface is plugged in. Neither knows
// about the other, so the correspondence is made here: **one of this
// computer's outputs per cable the patch plays to**. A patch sending its lead
// to USB 1 and its drums to DIN 1 therefore reaches two synths, the way it
// would out of the module.
//
// Three things this owns because nothing else can:
//
//   * **the mask is unpacked here.** The firmware sends one call carrying the
//     whole target mask - it has one cable per bit and no notion of a port
//     name - so splitting it into sends, and sending once when two cables
//     land on the same output, is the page's job.
//   * **a note that went out has to be taken back.** Re-pointing a cable at
//     another synth, or a synth being unplugged, leaves every note it was
//     holding sounding for ever: a MIDI note-off is the only thing that ends
//     a note, and the module's own note-off would arrive at the new port. So
//     what is sounding is tracked per cable and released before the route
//     changes under it.
//   * **a route is remembered by name**, not by the id Web MIDI makes up per
//     session, so the synth on the desk is still routed tomorrow. Two
//     interfaces reporting the same name are one as far as this can tell,
//     and the first wins.
//   * **a message is sent for when it happened**, not for when the page got
//     round to it. The passes that made it may have run in a burst at the end
//     of a frame, or late after a rebuild of the page; its simulated time is
//     where it belongs on this computer's clock (`EmbeddedModule.wallAt`),
//     and Web MIDI takes that time with the bytes, plus the same headroom the
//     audio listener keeps.
//
// Web MIDI is a browser limit, not an app limit: without it the module still
// runs, and the audio listener on the play tab is what it is heard through.

import * as P from '../protocol/generated.js';
import { MUSICAL_PORTS } from '../protocol/names.js';
import { OUTPUT_LATENCY_MS } from './module.js';
import { access } from './webmidi.js';

const NOTE_OFF = 0x80, NOTE_ON = 0x90;
const PROGRAM_CHANGE = 0xc0, CHANNEL_PRESSURE = 0xd0;
const REALTIME_FIRST = 0xf8;

// A MIDI message is one, two or three bytes by its status, and Web MIDI
// refuses one of the wrong length rather than truncating it - so a Program
// Change sent as three bytes is a throw, not a wrong note.
export function messageBytes(type, channel, d1, d2) {
  if (type >= REALTIME_FIRST) return [type];
  const status = (type & 0xf0) | ((channel - 1) & 0x0f);
  if (type === PROGRAM_CHANGE || type === CHANNEL_PRESSURE) return [status, d1 & 0x7f];
  return [status, d1 & 0x7f, d2 & 0x7f];
}

// The module's cables a patch plays out of: the union of its MIDI out nodes'
// target masks and the clock's own output mask, as the ports the app names
// them by. Only these are offered a route - eight rows, six of them for
// cables nothing is played on, is the panel saying "no" six times.
//
// The clock's mask is in it because a cable carrying nothing but clock is
// still a cable something is on: clocking a drum machine and playing it no
// notes is an ordinary patch, and without this the row it needs to reach the
// desk would never appear.
export function cablesOut(patch, caps = null, globals = null) {
  const slots = patch?.midiOut ?? [];
  const limit = caps?.midiOut ?? slots.length;
  let mask = globals?.clockOutMask ?? 0;
  for (const [i, port] of slots.entries()) if (i < limit) mask |= port.targetMask;
  return MUSICAL_PORTS.filter((cable) => (mask & cable.value) !== 0);
}

export class MidiOutputs {
  constructor(module, library = null) {
    this.module = module;
    this.library = library;
    this.access = null;
    // cable (a MidiPort bit) -> the name of the output it is played out of.
    this.routes = new Map();
    // cable -> the notes it has sounding, as `channel:pitch`, and where they
    // were sent. What a panic and a re-route have to release.
    this.sounding = new Map();
    this.onChange = () => {};
    this.restore();
    module.onMidi((event) => this.forward(event));
  }

  async connect() {
    this.access = await access();
    return this.access;
  }

  get outputs() { return this.access ? [...this.access.outputs.values()] : []; }

  // The name a cable is played out of, or '' for nowhere. A name with no port
  // behind it - the synth is unplugged, or this is another computer - is
  // still the route: it comes back the moment the port does.
  routeOf(cable) { return this.routes.get(cable) ?? ''; }

  output(cable) {
    const name = this.routeOf(cable);
    return name ? this.outputs.find((port) => port.name === name) ?? null : null;
  }

  // Point a cable at one of this computer's outputs, or at nothing. Whatever
  // it was holding is released first, on the port that is holding it.
  route(cable, name) {
    if (this.routeOf(cable) === name) return;
    this.release(cable);
    if (name) this.routes.set(cable, name); else this.routes.delete(cable);
    this.remember();
  }

  // Everything sounding, taken back. The play panel's own panic reaches the
  // module; this is for the notes already out of the page, which no message
  // to the module can end.
  panic() {
    for (const cable of [...this.sounding.keys()]) this.release(cable);
  }

  // One event the module played, to every output its target mask names.
  //
  // An output named twice - two cables, one synth - is sent to once: the
  // module puts one copy on each of its own cables, and two copies into one
  // port is a flam nobody programmed.
  forward({ t, target, type, d1, d2, channel }) {
    const bytes = messageBytes(type, channel, d1, d2);
    const at = this.stamp(t);
    const sent = new Set();
    for (const cable of MUSICAL_PORTS) {
      if ((target & cable.value) === 0) continue;
      const output = this.output(cable.value);
      if (!output) continue;
      this.track(cable.value, type, d1, d2, channel);
      if (sent.has(output.name)) continue;
      sent.add(output.name);
      this.send(output, bytes, at);
    }
  }

  // When a message at simulated `simUs` leaves the port, on the clock Web
  // MIDI takes (`performance.now()`): where it fell on the wall, plus the
  // headroom. A time already past sends at once, which is what Web MIDI does
  // with it and the most a late event can be.
  stamp(simUs) { return this.module.wallAt(simUs) + OUTPUT_LATENCY_MS; }

  // --- what is sounding ----------------------------------------------------

  track(cable, type, d1, d2, channel) {
    if (type !== NOTE_ON && type !== NOTE_OFF) return;
    const held = this.sounding.get(cable) ?? new Set();
    const key = `${channel}:${d1 & 0x7f}`;
    if (type === NOTE_ON && d2 > 0) held.add(key); else held.delete(key);
    if (held.size) this.sounding.set(cable, held); else this.sounding.delete(cable);
  }

  // The note-offs a cable owes, sent where its notes went. A note is released
  // with the channel and pitch it was played with, which is the same rule
  // `SoundingNotes` keeps in the firmware. Released at the module's own time:
  // everything it has sent so far is stamped no later than that, so a
  // note-off sent for then cannot overtake the note-on it ends.
  release(cable) {
    const held = this.sounding.get(cable);
    this.sounding.delete(cable);
    const output = this.output(cable);
    if (!held || !output) return;
    const at = this.stamp(this.module.now);
    for (const key of held) {
      const [channel, pitch] = key.split(':').map(Number);
      this.send(output, messageBytes(NOTE_OFF, channel, pitch, 0), at);
    }
  }

  send(output, bytes, at) {
    try {
      output.send(bytes, at);
    } catch {
      // A port that went away between the event and the send, or one that
      // refused the message. The panel redraws off `onChange` and says what
      // is reachable, so this needs no notice of its own.
    }
  }

  // --- kept in this browser -------------------------------------------------

  // Stored under the firmware's own enum keys rather than the bits, so a
  // renumbered MidiPort drops the routes it cannot mean any more instead of
  // pointing a different cable at a synth.
  remember() {
    const stored = {};
    for (const [cable, name] of this.routes) {
      const key = Object.keys(P.MidiPort).find((k) => P.MidiPort[k] === cable);
      if (key) stored[key] = name;
    }
    this.library?.writeMidiOut(stored);
  }

  restore() {
    for (const [key, name] of Object.entries(this.library?.readMidiOut() ?? {})) {
      const cable = P.MidiPort[key];
      if (cable && typeof name === 'string') this.routes.set(cable, name);
    }
  }
}
