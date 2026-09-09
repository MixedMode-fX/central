// The module, running in this page.
//
// This is the firmware compiled to WebAssembly with the browser standing in
// for the Teensy: the same object code the unit tests run, reached through the
// same hardware seam (`IGpio`, `IMidiOut`) the Teensy layer implements. There
// is no second implementation of anything here - no JavaScript validator, no
// JavaScript sequencer - which is what makes the app's "the module will accept
// this patch" a fact rather than a hope.
//
// It wears two hats, and that is the whole point of merging the editor and the
// emulator into one app:
//
//   * **a transport.** `Device` talks to a transport, not to Web MIDI, so the
//     wasm module is one: the app drives it over the real SysEx protocol,
//     through the same codec, and cannot tell it from hardware. Every edit the
//     patch tab makes reaches it as the message a cable would have carried.
//   * **a machine that runs.** It is the main loop, the interval timer and the
//     sync pin, as `main.cpp` and `teensy_clock.cpp` are on the module - so
//     the patch being edited is also the patch being *heard*, with its jacks,
//     its MIDI out and its sequencer positions live in the same page.
//
// Two reasons the embedded module matters beyond convenience:
//
//   * **Web MIDI does not exist on iOS at all**, and needs a permission prompt
//     and an OTG cable on Android. An app that could only reach a module over
//     Web MIDI would be unusable on most phones.
//   * It makes the app demonstrable and testable with no hardware, which is
//     what the Pages deployment is for.
//
// What it is *not*: a substitute for a module. Presets live in RAM, so a
// reload loses them (the patch library in `storage.js` is the answer to that),
// and the jacks go nowhere but the page.

import * as P from './protocol.js';

// Where mmmc.wasm might be. These resolve against *this module's* URL
// (app/src/module.js), not the page's, which is one level deeper than it
// looks - the reason to check this in a browser rather than reason about it:
//
//   ../../mmmc.wasm              /mmmc.wasm       the Pages layout
//   ../mmmc.wasm                 /app/...         a copy beside the page
//   ../../emulator/dist/...      the repository served from its root
export const CANDIDATE_PATHS = ['../../mmmc.wasm', '../mmmc.wasm', '../../emulator/dist/mmmc.wasm'];

// Simulated microseconds per pass, and the most simulated time one animation
// frame may advance. The module is a gate-rate machine: it wants passes, not
// wall-clock. One frame of real time is one frame of simulated time, capped so
// a backgrounded tab does not come back and run a minute of passes at once.
const MAX_FRAME_US = 100000;
const PASS_US = 1000;

// MIDI status bytes the page has to recognise to route an event the way
// main.cpp's loop does.
const NOTE_OFF = 0x80, NOTE_ON = 0x90, CONTROL_CHANGE = 0xb0, PROGRAM_CHANGE = 0xc0;

const MIDI_LOG_MAX = 200;

// What the scope draws: one column per millisecond of *simulated* time, four
// seconds of them, with levels OR-accumulated into the column so a pulse
// shorter than a column still shows. A gate is a 1 ms edge on this machine,
// and the whole point of a scope is that it does not miss one.
const TRACE_US = 1000;
const TRACE_LEN = 4000;

// What the piano roll draws: the notes of the last few seconds, in and out.
const ROLL_US = 8e6;
const ROLL_MAX = 512;

export class EmbeddedModule {
  constructor(exports) {
    this.E = exports;
    this.now = 0;
    this.clockInterval = 0;
    this.clockNextAt = 0;
    this.running = false;
    this.handler = () => {};
    this.midiListeners = new Set();
    this.frameListeners = new Set();
    this.passListeners = new Set();
    this.midiLog = [];
    // The log is a ring: once it is full its *length* stops changing, so
    // anything deciding "has anything happened?" from the length would decide
    // "no" for ever after the two hundredth event. This counts every event the
    // module has ever sent and never goes backwards.
    this.midiSeq = 0;
    this.notes = [];              // what the piano roll draws: notes, in and out
    this.accepted = null;         // ports that took the last delivered event

    // Sampled once per pass, which is the only rate that tells the truth: a
    // trigger is high for a millisecond or two and an animation frame is
    // sixteen, so anything polled per frame sees a lit gate roughly one time
    // in eight. `levels` is what is high *now*; `activity` is what has been
    // high at any pass since the page last painted, and the page ORs the two.
    this.levels = { jackIn: 0, jackOut: 0, gate: 0, green: 0, red: 0 };
    this.activity = { jackIn: 0, jackOut: 0, gate: 0, green: 0, red: 0 };
    // The scope's ring buffer, and the column being accumulated into it.
    this.trace = {
      us: TRACE_US, len: TRACE_LEN, head: 0, filled: 0, at: 0,
      jackIn: new Uint8Array(TRACE_LEN),
      jackOut: new Uint8Array(TRACE_LEN),
      gate: new Uint32Array(TRACE_LEN),
    };
    this.column = { jackIn: 0, jackOut: 0, gate: 0, until: 0 };
    // Per jack, what the page is driving into it: a held level, a pulse that
    // ends at a simulated time, or a free-running square wave.
    this.jackSources = Array.from({ length: P.GPIO_N }, () => ({ level: 0, pulseUntil: 0, hz: 0 }));
    this.syncHz = 0;              // the CV sync generator, off unless asked for
    this.syncHigh = false;

    // main.cpp stirs the entropy pool *before* any node is constructed: a node
    // that draws a seed at construction (RandomSequencer, Probability) must not
    // play the same thing on every power cycle. emu_boot() builds the patch, so
    // this has to happen first.
    for (let i = 0; i < 4; i++) this.E.emu_entropy_stir((Math.random() * 0x100000000) >>> 0);
    this.E.emu_entropy_stir(Date.now() >>> 0);
    this.E.emu_boot(0);
  }

  static async load(url) {
    const bytes = await fetchWasm(url);
    const module = { instance: null };
    const { instance } = await WebAssembly.instantiate(bytes, {
      // Everything the module plays. The editor half of this app has nowhere to
      // put musical MIDI; the play tab, the audio listener and a real MIDI
      // output all do, so it is handed to whoever is listening.
      env: {
        mmmc_midi_send: (target, type, d1, d2, channel) => {
          module.instance?.emitMidi(target, type, d1, d2, channel);
        },
      },
    });
    if (instance.exports.__wasm_call_ctors) instance.exports.__wasm_call_ctors();
    module.instance = new EmbeddedModule(instance.exports);
    return module.instance;
  }

  // --- the transport Device wants ----------------------------------------

  onMessage(fn) { this.handler = fn; }

  send(bytes) {
    const E = this.E;
    if (bytes.length > E.emu_sysex_in_capacity()) {
      throw new Error('that message is longer than the module will accept');
    }
    const scratch = E.emu_sysex_in_ptr();
    new Uint8Array(E.memory.buffer).set(bytes, scratch);
    E.emu_sysex_out_clear();
    E.emu_sysex_in(E.emu_const_control_port(), scratch, bytes.length);

    // Replies come back as one run of complete messages; split it on F0/F7.
    const out = new Uint8Array(E.memory.buffer, E.emu_sysex_out_ptr(), E.emu_sysex_out_len());
    const replies = [];
    let start = -1;
    for (let i = 0; i < out.length; i++) {
      if (out[i] === 0xf0) start = i;
      else if (out[i] === 0xf7 && start >= 0) { replies.push(out.slice(start, i + 1)); start = -1; }
    }
    // Delivered on a microtask, the way a real port would: the Device API is
    // async, and code that only works because a reply arrived synchronously
    // would break the moment it met real hardware.
    queueMicrotask(() => { for (const reply of replies) this.handler(reply); });
  }

  get name() { return 'the built-in module'; }
  get deviceId() { return P.SYSEX_DEFAULT_DEVICE; }

  // --- keeping it alive ---------------------------------------------------

  // The module is a machine that runs passes. Without this it would answer
  // SysEx and do nothing else: no beat on the green LED, no sequencer moving,
  // no debounced autosave, no quantised patch swap ever arriving.
  start() {
    if (this.running) return;
    this.running = true;
    let last = 0;
    const frame = (ts) => {
      if (!this.running) return;
      const dt = last ? Math.min(MAX_FRAME_US, (ts - last) * 1000) : 0;
      last = ts;
      for (const listener of this.frameListeners) listener(this.now);
      this.advance(dt);
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  stop() { this.running = false; }

  // Called at the top of every animation frame, before the passes it will run.
  // The audio listener uses it to anchor simulated time to the audio clock.
  onFrame(fn) { this.frameListeners.add(fn); return () => this.frameListeners.delete(fn); }

  // Called after every pass. A gate that goes high and low again between two
  // animation frames is invisible to anything watching per frame, and a
  // trigger is exactly that shape, so anything reacting to edges - the audio
  // listener's clicks - has to look here.
  onPass(fn) { this.passListeners.add(fn); return () => this.passListeners.delete(fn); }

  advance(microseconds) {
    const E = this.E;
    const passes = Math.floor(microseconds / PASS_US);
    for (let i = 0; i < passes; i++) {
      this.driveJacks();
      this.driveClock();
      E.emu_control_service(this.now);
      E.emu_pass(this.now);
      // Before the listeners, so anything reacting to an edge - the audio
      // listener's clicks - reads the levels of the pass it is reacting to.
      this.sample();
      for (const listener of this.passListeners) listener(this.now);
      this.now += PASS_US;
    }
  }

  // Everything the page can show about a running module, read once per pass.
  //
  // This is the fix for lights that were "far from time accurate": the jacks,
  // the gate buses and the LEDs used to be read by whatever was painting, at
  // 10 Hz. A gate that goes high and low again inside one of those windows -
  // which is every trigger this machine makes - was invisible unless the poll
  // happened to land on it, so the dots lit at random and the pattern they
  // showed was not the pattern being played. Sampled per pass, an edge cannot
  // be missed: it can only be *late*, by at most one animation frame.
  sample() {
    const E = this.E;
    let jackIn = 0;
    let jackOut = 0;
    for (let j = 0; j < P.GPIO_N; j++) {
      if (E.emu_jack_input(j)) jackIn |= 1 << j;
      if (E.emu_jack_output(j)) jackOut |= 1 << j;
    }
    const gate = E.emu_gate_buses();
    const green = E.emu_led(0);
    const red = E.emu_led(1);
    this.levels = { jackIn, jackOut, gate, green, red };

    const a = this.activity;
    a.jackIn |= jackIn; a.jackOut |= jackOut; a.gate |= gate;
    if (green > a.green) a.green = green;
    if (red > a.red) a.red = red;

    const c = this.column;
    c.jackIn |= jackIn; c.jackOut |= jackOut; c.gate |= gate;
    const t = this.trace;
    if (this.now >= c.until) {
      t.jackIn[t.head] = c.jackIn;
      t.jackOut[t.head] = c.jackOut;
      t.gate[t.head] = c.gate;
      t.head = (t.head + 1) % t.len;
      if (t.filled < t.len) t.filled++;
      t.at = this.now;
      c.jackIn = jackIn; c.jackOut = jackOut; c.gate = gate;
      c.until = this.now + t.us;
    }
  }

  // What has been high since the page last painted, folded together with what
  // is high now: a held level stays lit, and a two-millisecond trigger between
  // two frames lights for the frame it belongs to rather than not at all.
  takeActivity() {
    const a = this.activity;
    this.activity = { jackIn: 0, jackOut: 0, gate: 0, green: 0, red: 0 };
    const now = this.levels;
    return {
      jackIn: a.jackIn | now.jackIn,
      jackOut: a.jackOut | now.jackOut,
      gate: a.gate | now.gate,
      green: Math.max(a.green, now.green),
      red: Math.max(a.red, now.red),
    };
  }

  // What the page holds each input jack at. An output jack is driven by the
  // firmware and never written here.
  driveJacks() {
    for (let j = 0; j < P.GPIO_N; j++) {
      const source = this.jackSources[j];
      if (source.pulseUntil && this.now >= source.pulseUntil) {
        source.pulseUntil = 0;
        this.E.emu_jack_set_input(j, source.level ? 1 : 0);
      }
      if (source.hz && !source.pulseUntil) {
        const period = 1e6 / source.hz;
        this.E.emu_jack_set_input(j, (this.now % period) < period / 2 ? 1 : 0);
      }
    }
  }

  // teensy_clock.cpp: reprogram the timer when the clock asks for it, else let
  // it free-run. The page is that timer.
  driveClock() {
    const E = this.E;
    if (E.emu_clock_take_interval_change()) {
      this.clockInterval = E.emu_clock_interval_us();
      this.clockNextAt = this.now + this.clockInterval;
    }
    while (this.clockInterval && this.now >= this.clockNextAt) {
      E.emu_clock_advance();
      this.clockNextAt += this.clockInterval;
    }
    // The sync jack, when the page has been asked to pulse it: the one thing a
    // CV-clocked patch cannot try without a cable.
    if (this.syncHz && E.emu_clock_source() === 1) {
      const period = 1e6 / this.syncHz;
      const high = (this.now % period) < period / 2;
      if (high && !this.syncHigh) E.emu_sync_edge(this.now);
      this.syncHigh = high;
    }
  }

  // --- musical MIDI -------------------------------------------------------

  onMidi(fn) { this.midiListeners.add(fn); return () => this.midiListeners.delete(fn); }

  emitMidi(target, type, d1, d2, channel) {
    const event = { t: this.now, target, type, d1, d2, channel };
    this.midiLog.push(event);
    this.midiSeq++;
    if (this.midiLog.length > MIDI_LOG_MAX) this.midiLog.shift();
    this.rollNote('out', event);
    for (const listener of this.midiListeners) listener(event);
  }

  clearMidiLog() { this.midiLog.length = 0; }

  // The piano roll's notes: a note on opens one, the matching note off closes
  // it, and one still open is drawn as far as the playhead. Kept by simulated
  // time, like everything else the page draws, so a burst of passes inside one
  // animation frame lands where it happened rather than where it was noticed.
  rollNote(direction, { t, type, d1, d2, channel, target }) {
    if (!this.isNote(type)) return;
    const on = type === NOTE_ON && d2 > 0;
    const open = this.notes.find((n) => n.end === null && n.pitch === d1
      && n.channel === channel && n.direction === direction);
    if (on) {
      if (open) open.end = t;
      this.notes.push({ direction, pitch: d1, velocity: d2, channel, port: target, start: t, end: null });
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

  // One incoming message, offered to the module exactly as main.cpp's loop
  // offers it: preset recall first, then NRPN, then the controller bindings,
  // and only what is left reaches the graph. Getting this order right is what
  // makes "learn" work with a controller plugged into the browser rather than
  // into the module.
  //
  // Returns how many ports accepted it, or null when it was consumed by the
  // control plane - which is a visible answer to "why did nothing happen?".
  deliverMidi(port, type, channel, d1 = 0, d2 = 0) {
    const E = this.E;
    if (type === PROGRAM_CHANGE && E.emu_control_program_change(port, channel, d1, this.now)) {
      this.accepted = null;
      return null;
    }
    if (type === CONTROL_CHANGE && E.emu_control_cc(port, channel, d1, d2, this.now)) {
      this.accepted = null;
      return null;
    }
    this.accepted = E.emu_deliver_midi(port, type, channel, d1, d2, this.now);
    // Into the roll as well, so the keyboard, a controller and the patch's own
    // output are all on one time line: what went in, and what came out of it.
    this.rollNote('in', { t: this.now, type, d1, d2, channel, target: port });
    return this.accepted;
  }

  // A realtime byte (clock, start, stop, continue) goes straight in: it has no
  // channel and no data, and the clock is what reads it.
  deliverRealtime(port, status) {
    this.E.emu_deliver_midi(port, status, 0, 0, 0, this.now);
  }

  isNote(type) { return type === NOTE_ON || type === NOTE_OFF; }

  // --- the seam, page side ------------------------------------------------

  jackMode(jack) { return this.E.emu_jack_mode(jack); }          // 0 unused, 1 in, 2 out
  jackOutput(jack) { return this.E.emu_jack_output(jack); }
  jackInput(jack) { return this.E.emu_jack_input(jack); }

  setJackInput(jack, level) {
    const source = this.jackSources[jack];
    source.level = level ? 1 : 0;
    source.pulseUntil = 0;
    this.E.emu_jack_set_input(jack, source.level);
  }

  // A tap: high for a few milliseconds of simulated time and back down. A gate
  // is an edge, so a button that only toggled would need two presses to make
  // one trigger.
  pulseJack(jack, milliseconds = 20) {
    const source = this.jackSources[jack];
    source.pulseUntil = this.now + milliseconds * 1000;
    this.E.emu_jack_set_input(jack, 1);
  }

  setJackRate(jack, hz) {
    const source = this.jackSources[jack];
    source.hz = hz;
    if (!hz) this.E.emu_jack_set_input(jack, source.level);
  }

  jackRate(jack) { return this.jackSources[jack].hz; }

  syncPulse() { this.E.emu_sync_edge(this.now); }
  setSyncRate(hz) { this.syncHz = hz; }

  clockStart() { this.E.emu_clock_start(); }
  clockStop() { this.E.emu_clock_stop(); }
  clockResume() { this.E.emu_clock_resume(); }

  clock() {
    return {
      bpm: this.E.emu_clock_bpm(),
      running: this.E.emu_clock_running() === 1,
      source: this.E.emu_clock_source(),
      count: this.E.emu_clock_count(),
    };
  }

  // The module's entire feedback surface, which a page can show and a MIDI
  // cable cannot.
  leds() { return { green: this.E.emu_led(0), red: this.E.emu_led(1) }; }

  // Which gate buses are high this pass: the live view of a running patch the
  // module itself has no way to display.
  gateBuses() { return this.E.emu_gate_buses(); }

  // Where each loaded sequencer is, by node index in the running patch. This
  // is what puts a playhead on the step grids while they are being edited.
  seqKind(node) { return this.E.emu_seq_kind(node); }
  seqLanes(node) { return this.E.emu_seq_lanes(node); }
  seqPosition(node, lane) { return this.E.emu_seq_position(node, lane); }
  nodeCount() { return this.E.emu_node_count(); }

  version() {
    const memory = new Uint8Array(this.E.memory.buffer);
    let out = '';
    for (let i = this.E.emu_version(); memory[i] !== 0; i++) out += String.fromCharCode(memory[i]);
    return out;
  }
}

// The module comes from next to the page when there is a server, and from the
// copy the build embeds when there is not - which is what lets the built page
// be opened from a file:// URL with nothing serving it.
async function fetchWasm(url) {
  // On a file:// page there is nothing to fetch from and every attempt is a
  // console error, so the copy the build embedded is tried first there.
  const embedded = () => inlineWasm();
  if (!url && globalThis.location?.protocol === 'file:') {
    const bytes = embedded();
    if (bytes) return bytes;
  }
  const urls = url ? [url] : CANDIDATE_PATHS;
  let lastError = null;
  for (const candidate of urls) {
    try {
      const response = await fetch(new URL(candidate, import.meta.url));
      if (!response.ok) { lastError = new Error(`${candidate}: ${response.status}`); continue; }
      return await response.arrayBuffer();
    } catch (error) {
      lastError = error;
    }
  }
  const bytes = embedded();
  if (bytes) return bytes;
  throw new Error(`could not load the module (${lastError?.message ?? 'not found'})`);
}

// The copy build.sh writes into the page, base64. Absent when the page is
// served from the source tree, which is the case that fetches instead.
function inlineWasm() {
  const text = globalThis.document?.getElementById('mmmc-wasm')?.textContent?.trim();
  if (!text || text.startsWith('/*')) return null;
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}
