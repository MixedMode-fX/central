// The module, watched: the app's end of the monitor (src/monitor/monitor.h).
//
// Everything on the page that moves without an edit - the LEDs, the gate
// dots, the lit wires, the jack lamps, the scope, the piano rolls, the
// playheads, the MIDI log - is painted from here, and this is fed by exactly
// one thing: the frames the module answers a SYSEX_MONITOR_REQUEST with,
// decoded by `Device.monitorFrame` and handed to `ingest`. It does not know
// whether the module is in this page, in a plugin or on a cable, and neither
// does anything that reads it. That is the point: the module has no display,
// this app is its display wherever it is, and a display is fed by what the
// instrument reports rather than by reading its memory.
//
// **What is live is sampled by the module, not polled by the page.** A
// trigger on this machine is high for a millisecond or two and a frame is
// sixteen, so a page that read the levels at paint time saw a pattern nobody
// was playing. The module folds every pass into the frame - what has been
// high since the last one, the brightest each LED has been, every note that
// crossed a watched bus or left on a cable with the pass it happened on - so
// an edge is never missed here; it can only be late, by one frame.
//
// **Time is the module's.** Every frame says which pass it ends on and every
// note in it says how long before that it happened, so the roll and the
// scope are drawn in the module's own clock whatever cable the frame came
// down and however late it arrived.

import * as P from '../protocol/generated.js';

const NOTE_OFF = 0x80, NOTE_ON = 0x90;

// No note-on has been seen on a bus. Not a pitch: MIDI notes stop at 127.
export const NO_NOTE = 0xff;
// A position that is not one: a sequencer before its first advance, a loop
// slot the walk has not reached. The firmware's own byte (Monitor::NONE),
// which the wire carries as 0x7F because 0xFF is not a data byte.
export const NO_POSITION = 0xff;

// What the scope draws: one column per frame, each covering the passes
// between the frame before it and itself, with the gates and jacks that were
// high at any pass in it and the level each CV bus had at its end. Four
// seconds of them are drawn; this many columns is longer than that at any
// frame rate the page paints at, so the window is always full.
const TRACE_LEN = 1024;
const TRACE_US = 4e6;

// What the piano roll draws: the notes of the last few seconds.
const ROLL_US = 8e6;
const ROLL_MAX = 512;

const MIDI_LOG_MAX = 200;

const blank = () => ({ jackIn: 0, jackOut: 0, gate: 0, green: 0, red: 0 });

export class Monitor {
  constructor() {
    this.reset();
  }

  // Nothing seen yet. A new transport is a new module, and what the last
  // one did is not what this one is doing.
  reset() {
    // Module time at the end of the last frame, in microseconds and
    // unwrapped: the module counts in 32 bits and this page may be open for
    // longer than they hold.
    this.now = 0;
    this.lastAt = null;
    this.frames = 0;
    this.clock = { running: false, bpm: 0, count: 0 };
    // `levels` is what is high at the end of the last frame; `activity` is
    // what has been high at any pass since the page last painted.
    this.levels = blank();
    this.activity = blank();
    this.lost = false;
    this.trace = {
      len: TRACE_LEN, us: TRACE_US, head: 0, filled: 0,
      t: new Float64Array(TRACE_LEN),
      jackIn: new Uint8Array(TRACE_LEN),
      jackOut: new Uint8Array(TRACE_LEN),
      gate: new Uint32Array(TRACE_LEN),
      cv: Array.from({ length: P.N_CV_BUS }, () => new Int16Array(TRACE_LEN)),
    };
    this.notes = [];              // what the piano roll draws: notes in, out and on the buses
    // The last note-on each note bus carried, or NO_NOTE where it has
    // carried none. "Last note-on wins" is the rule every node with a root
    // inlet follows, so this is what such a node is rooted on - read off the
    // bus itself rather than out of the node, which is what makes it one
    // answer for every algorithm that has a root.
    this.busNotes = new Uint8Array(P.N_NOTE_BUS).fill(NO_NOTE);
    // Which note buses somebody is reading, and how many of them. A bus is
    // only asked for when something is listening to it: the module does not
    // read a bus nobody wants.
    this.watch = new Map();
    this.noteListeners = new Set();
    // Where each node the last frame was asked about is, by node index.
    this.positions = new Map();
    // What left the module, most recent last. A ring: once it is full its
    // length never changes, so `midiSeq` counts every event ever logged and
    // is what a view watches.
    this.midiLog = [];
    this.midiSeq = 0;
  }

  // --- what to ask for ------------------------------------------------------

  get noteMask() {
    let mask = 0;
    for (const bus of this.watch.keys()) mask |= 1 << bus;
    return mask & 0xffff;
  }

  get watchedBuses() { return new Set(this.watch.keys()); }

  watchNoteBus(bus) { this.watch.set(bus, (this.watch.get(bus) ?? 0) + 1); }

  unwatchNoteBus(bus) {
    const held = this.watch.get(bus);
    if (!held) return;
    if (held > 1) { this.watch.set(bus, held - 1); return; }
    this.watch.delete(bus);
    // Nobody is reading it any more, which in the patch means nothing writes
    // it: the note it was rooted on is history, not the answer to the next
    // question about it.
    this.busNotes[bus] = NO_NOTE;
  }

  // Every note-on and note-off seen on a watched bus, as it arrives: what
  // the audio listener plays a bus from.
  onNote(fn) { this.noteListeners.add(fn); return () => this.noteListeners.delete(fn); }

  // The note a bus is rooted on: the last note-on it carried, or NO_NOTE.
  busNote(bus) {
    return bus >= 0 && bus < this.busNotes.length ? this.busNotes[bus] : NO_NOTE;
  }

  // Where a node is, as the last frame reported it: a step per lane for a
  // sequencer; the degree sounding, the loop slot sounding and the loop's
  // chords for a harmony. Empty for a node with no position, or one the
  // frame was not asked about.
  positionsOf(node) { return this.positions.get(node) ?? []; }

  // --- one frame --------------------------------------------------------------

  ingest(frame) {
    // Module time only goes forward: a frame from a module that has been
    // rebooted, or one from before a wrap, sits after the last one.
    if (this.lastAt !== null) {
      const step = frame.at - this.lastAt;
      this.now += step >= 0 ? step : step + 0x100000000;
    } else {
      this.now = frame.at;
    }
    this.lastAt = frame.at;
    this.frames++;
    this.clock = frame.clock;
    this.lost = frame.lost;

    this.levels = {
      jackIn: frame.jackIn.now, jackOut: frame.jackOut.now, gate: frame.gate.now,
      green: frame.green, red: frame.red,
    };
    const a = this.activity;
    a.jackIn |= frame.jackIn.since;
    a.jackOut |= frame.jackOut.since;
    a.gate |= frame.gate.since;
    a.green = Math.max(a.green, frame.green);
    a.red = Math.max(a.red, frame.red);

    const t = this.trace;
    t.t[t.head] = this.now;
    t.jackIn[t.head] = frame.jackIn.since;
    t.jackOut[t.head] = frame.jackOut.since;
    t.gate[t.head] = frame.gate.since;
    for (let b = 0; b < P.N_CV_BUS; b++) t.cv[b][t.head] = frame.cv[b] ?? 0;
    t.head = (t.head + 1) % t.len;
    if (t.filled < t.len) t.filled++;

    for (const { node, values } of frame.nodes) {
      this.positions.set(node, values.map((v) => (v === 0x7f ? NO_POSITION : v)));
    }

    for (const e of frame.events) {
      const at = this.now - e.ageMs * 1000;
      if (e.bus !== null) {
        const event = { t: at, bus: e.bus, type: e.type, channel: e.channel, d1: e.d1, d2: e.d2 };
        if (e.type === NOTE_ON && e.d2) this.busNotes[e.bus] = e.d1;
        this.rollNote('bus', event);
        for (const listener of this.noteListeners) listener(event);
      } else {
        const event = { t: at, target: e.target, type: e.type, channel: e.channel, d1: e.d1, d2: e.d2 };
        this.midiLog.push(event);
        this.midiSeq++;
        if (this.midiLog.length > MIDI_LOG_MAX) this.midiLog.shift();
        this.rollNote('out', event);
      }
    }
  }

  // What has been high since the page last painted, folded together with
  // what is high now: a held level stays lit, and a two-millisecond trigger
  // between two frames lights for the frame it belongs to rather than not at
  // all.
  takeActivity() {
    const a = this.activity;
    this.activity = blank();
    const now = this.levels;
    return {
      jackIn: a.jackIn | now.jackIn,
      jackOut: a.jackOut | now.jackOut,
      gate: a.gate | now.gate,
      green: Math.max(a.green, now.green),
      red: Math.max(a.red, now.red),
    };
  }

  // --- the piano roll ----------------------------------------------------------

  // A note played into the module from this page - the keyboard, the pads, a
  // controller - onto the roll beside what came out of it. What a DAW track
  // or a DIN keyboard plays into a module elsewhere shows on the buses it
  // reaches, not here: this is what *this page* played, wherever the module is.
  playedIn(port, type, channel, d1, d2, t = this.now) {
    this.rollNote('in', { t, type, d1, d2, channel, target: port });
  }

  // The piano roll's notes: a note on opens one, the matching note off closes
  // it, and one still open is drawn as far as the playhead. Kept by module
  // time, like everything else drawn here, so a burst of passes inside one
  // frame lands where it happened rather than where it was noticed.
  //
  // A note is filed under *where it was seen*: played into the module, sent
  // out of it, or on note bus N. The same note usually appears more than once
  // - a sequencer writes it to a bus and a MIDI out node then sends it - and
  // that is the point: the roll shows the same phrase at each point in the
  // chain, so a bus carrying what you did not expect is visible against the
  // one that does.
  rollNote(direction, { t, type, d1, d2, channel, target, bus = null }) {
    if (type !== NOTE_ON && type !== NOTE_OFF) return;
    const on = type === NOTE_ON && d2 > 0;
    const open = this.notes.find((n) => n.end === null && n.pitch === d1
      && n.channel === channel && n.direction === direction && n.bus === bus);
    if (on) {
      if (open) open.end = t;
      this.notes.push({ direction, bus, pitch: d1, velocity: d2, channel,
                        port: target ?? null, start: t, end: null });
      if (this.notes.length > ROLL_MAX) this.notes.splice(0, this.notes.length - ROLL_MAX);
    } else if (open) {
      open.end = t;
    }
    // Anything wholly older than the window is off the left edge for good.
    const cutoff = t - ROLL_US;
    while (this.notes.length && this.notes[0].end !== null && this.notes[0].end < cutoff) {
      this.notes.shift();
    }
  }

  rollSpan() { return ROLL_US; }

  clearNotes() { this.notes.length = 0; }

  clearMidiLog() { this.midiLog.length = 0; }
}
