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

import * as P from '../protocol/generated.js';
import { splitSysex } from '../protocol/codec.js';

// Simulated microseconds per pass. The module is a gate-rate machine: it
// wants passes, and the page owes it one per millisecond of wall clock.
const PASS_US = 1000;

// **Simulated time is wall time.** `now` follows `performance.now()` from an
// origin fixed when the loop starts, and every tick runs the passes the wall
// clock owes. Not "one frame of real time is one frame of simulated time":
// a frame is 16.667 ms and a pass is a millisecond, and flooring each frame to
// whole passes threw the odd fraction away every time - four percent of the
// tempo at 60 Hz, thirteen at 144 Hz, and a different amount whenever a frame
// came late. Against a fixed origin the fraction is simply still owed.
//
// The passes are run from a timer while the page is visible as well as from
// the animation frame, so the module is never more than a few milliseconds
// behind between frames; the frame is where the page paints what they did.
// Hidden, the page has neither, and the heartbeat runs them at this rate
// instead (`runtime/heartbeat.js`).
export const TICK_MS = 4;
// Further behind than this the module skips rather than catches up: a tab
// coming back from the background does not run a minute of passes at once,
// and a stall long enough to be a hole is a hole rather than every note of
// it played in the same instant.
const MAX_CATCHUP_US = 250000;
// How far ahead of the wall clock a render may run the module (`runAhead`).
const MAX_RUN_AHEAD_US = 150000;

// How far ahead of wall time the module's outputs are scheduled: a note the
// module plays at simulated `t` sounds, and leaves the MIDI port, at
// `wallAt(t)` plus this. It is the page's headroom against its own main
// thread. Passes run on the main thread, and so does everything else the
// page does; when a rebuild of the page takes forty milliseconds, the passes
// those milliseconds owed run afterwards, and their notes are still in the
// future by however much of this is left. It is also the latency of a key
// pressed on screen, which is what keeps it from being larger.
export const OUTPUT_LATENCY_MS = 40;

// MIDI status bytes the page has to recognise to route an event the way
// main.cpp's loop does.
const NOTE_OFF = 0x80, NOTE_ON = 0x90, CONTROL_CHANGE = 0xb0, PROGRAM_CHANGE = 0xc0;

const MIDI_LOG_MAX = 200;

// No note-on has been seen on a bus. Not a pitch: MIDI notes stop at 127.
export const NO_NOTE = 0xff;

// What the scope draws: one column per millisecond of *simulated* time, four
// seconds of them, with levels OR-accumulated into the column so a pulse
// shorter than a column still shows. A gate is a 1 ms edge on this machine,
// and the whole point of a scope is that it does not miss one. A CV bus is a
// level rather than an edge, so its column holds the last value the pass
// left there - and every CV bus is traced, because reading eight int16s a
// pass is cheaper than deciding which ones to read.
const TRACE_US = 1000;
const TRACE_LEN = 4000;

// What the piano roll draws: the notes of the last few seconds, in and out.
const ROLL_US = 8e6;
const ROLL_MAX = 512;

export class EmbeddedModule {
  constructor(exports) {
    this.E = exports;
    this.now = 0;
    // The wall time (`performance.now()`, ms) at which simulated time was
    // zero. Fixed by `start()` and moved only by `syncTo()`.
    this.origin = 0;
    this.timer = null;
    this.clockInterval = 0;
    this.clockNextAt = 0;
    this.running = false;
    this.handler = () => {};
    this.sysexDropped = 0;
    this.midiListeners = new Set();
    this.frameListeners = new Set();
    this.passListeners = new Set();
    this.noteBusListeners = new Set();
    // Which note buses somebody is reading. A bus is only read when something
    // is listening to it: reading all eight every pass is eight calls into the
    // module a millisecond for buses nobody is monitoring.
    this.noteBusWatch = new Map();     // bus -> how many listeners want it
    // The last note-on each note bus carried, or NO_NOTE where it has carried
    // none. "Last note-on wins" is the rule every node with a root inlet
    // follows, so this is what such a node is rooted on - read off the bus
    // itself rather than out of the node, which is what makes it one answer
    // for every algorithm that has a root rather than one export apiece.
    this.busNotes = new Uint8Array(P.N_NOTE_BUS).fill(NO_NOTE);
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
      cv: Array.from({ length: P.N_CV_BUS }, () => new Int16Array(TRACE_LEN)),
    };
    this.column = { jackIn: 0, jackOut: 0, gate: 0, cv: new Int16Array(P.N_CV_BUS), until: 0 };
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

  // From the module's bytes - which `runtime/wasm.js` fetches or decodes,
  // depending on how the page was served - to a running instance.
  static async instantiate(bytes) {
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

  // What the module said on its own, if anything: a Program Change recall, a
  // CC learn, an error behind the red LED. Nobody asked for these and nothing
  // is waiting on them, so without draining them the page would simply never
  // hear the module speak first - and worse, the next request would clear the
  // buffer they were sitting in. A real port streams them out as they happen;
  // here the page has to come and look, once a frame and before every
  // request.
  //
  // Delivered on a microtask, exactly as a reply is: the handler for an event
  // goes on to ask the module for a dump, and that must not re-enter `send`
  // from inside it.
  drainEvents() {
    const E = this.E;
    if (!E.emu_sysex_out_len()) return;
    const out = new Uint8Array(E.memory.buffer, E.emu_sysex_out_ptr(), E.emu_sysex_out_len());
    const messages = splitSysex(out).map((message) => message.slice());
    E.emu_sysex_out_clear();
    queueMicrotask(() => { for (const message of messages) this.handler(message); });
  }

  send(bytes) {
    const E = this.E;
    if (bytes.length > E.emu_sysex_in_capacity()) {
      throw new Error('that message is longer than the module will accept');
    }
    // Anything the module has already said, before this request's own replies
    // take the buffer over.
    this.drainEvents();
    const scratch = E.emu_sysex_in_ptr();
    new Uint8Array(E.memory.buffer).set(bytes, scratch);
    E.emu_sysex_out_clear();
    E.emu_sysex_in(E.emu_const_control_port(), scratch, bytes.length, this.now);

    // A reply the buffer had no room for is *dropped*, and a truncated run of
    // replies reads upstream as "the answer never completed" - which says
    // nothing about where it went. The counter is cumulative, so a rise since
    // the last message is this message's loss, and it is worth failing on:
    // the emulator's buffer is a browser-side number, and the module it is
    // standing in for streams to a port and never drops anything here.
    const lost = E.emu_sysex_out_dropped();
    if (lost > this.sysexDropped) {
      this.sysexDropped = lost;
      throw new Error('the module\'s reply did not fit its SysEx buffer; raise SYSEX_BUFFER');
    }

    // Replies come back as one run of complete messages. Copied out of the
    // module's memory, which the next call overwrites.
    const out = new Uint8Array(E.memory.buffer, E.emu_sysex_out_ptr(), E.emu_sysex_out_len());
    const replies = splitSysex(out).map((reply) => reply.slice());
    // Taken, so the buffer is empty again and the next thing found in it is
    // the module speaking first rather than this request's answer a second
    // time (`drainEvents`).
    E.emu_sysex_out_clear();
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
  //
  // Three clocks, one job, and only ever one of them running.
  //
  //   * **the timer**, while the page is visible. It keeps the module within
  //     a few milliseconds of the wall clock whether or not a frame is due,
  //     which is what puts its outputs on their clocks early
  //     (`OUTPUT_LATENCY_MS`).
  //   * **the animation frame**, which runs the same catch-up and then lets
  //     the page paint what the passes did.
  //   * **the heartbeat** (`runtime/heartbeat.js`), while the page is hidden.
  //     Neither of the other two is a clock there - the animation frame does
  //     not fire in a hidden page at all, and its timers are clamped to a
  //     wake-up a second - so the timer stands aside rather than running a
  //     quarter-second burst of passes once a second, and the heartbeat, whose
  //     clock is the audio render thread, runs them at real time instead.
  //
  // Where the heartbeat cannot beat (no gesture yet, no AudioWorklet, a phone
  // that suspends a backgrounded tab's audio outright) a hidden page has no
  // clock at all: the module is found where it was left and skips forward.
  start() {
    if (this.running) return;
    this.running = true;
    this.syncTo(performance.now());
    this.timer = setInterval(() => { if (!globalThis.document?.hidden) this.tick(); }, TICK_MS);
    const frame = () => {
      if (!this.running) return;
      this.advanceTo(performance.now());
      for (const listener of this.frameListeners) listener(this.now);
      this.drainEvents();
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  // One round of catch-up, from whichever clock is driving the module. The
  // heartbeat calls this and nothing else.
  tick() {
    if (!this.running) return;
    this.advanceTo(performance.now());
    this.drainEvents();
  }

  stop() {
    this.running = false;
    clearInterval(this.timer);
    this.timer = null;
  }

  // --- the time base --------------------------------------------------------

  // Simulated `now` is wall time `wallMs`, from here on.
  syncTo(wallMs) { this.origin = wallMs - this.now / 1000; }

  // Where simulated time `simUs` falls on the wall clock, in the domain of
  // `performance.now()`. This is what puts a note the module played on the
  // audio clock and on a MIDI port's clock: both are offsets from it.
  wallAt(simUs) { return this.origin + simUs / 1000; }

  // Run every pass owed up to wall time `wallMs`. A late frame runs the
  // passes it owes and lands on time; a frame's odd fraction of a millisecond
  // stays owed rather than being dropped; and a gap longer than
  // MAX_CATCHUP_US is skipped, so a tab that was in the background does not
  // play everything it missed in one instant. Nothing runs when the module is
  // already ahead of `wallMs` (`runAhead`).
  advanceTo(wallMs) {
    let owed = (wallMs - this.origin) * 1000 - this.now;
    if (owed > MAX_CATCHUP_US) {
      this.syncTo(wallMs - MAX_CATCHUP_US / 1000);
      owed = MAX_CATCHUP_US;
    }
    if (owed >= PASS_US) this.advance(owed);
  }

  // Run the module up to `ms` ahead of the wall clock, now. For a stall the
  // page can see coming - a rebuild of itself - so that the passes the stall
  // would have delayed are run before it and their outputs are scheduled
  // where they belong. The module then waits for the wall clock to catch up,
  // and nothing about the outputs' timing changes: they are early to be
  // computed, not early to sound.
  runAhead(ms) {
    if (!this.running) return;
    const ahead = Math.min(ms * 1000, MAX_RUN_AHEAD_US);
    if (ahead > 0) this.advanceTo(performance.now() + ahead / 1000);
  }

  // Called once per animation frame, after the passes owed to it have run and
  // before the page paints. The audio listener uses it to pair the wall clock
  // with the audio clock.
  onFrame(fn) { this.frameListeners.add(fn); return () => this.frameListeners.delete(fn); }

  // Called after every pass. A gate that goes high and low again between two
  // animation frames is invisible to anything watching per frame, and a
  // trigger is exactly that shape, so anything reacting to edges - the audio
  // listener's clicks - has to look here.
  onPass(fn) { this.passListeners.add(fn); return () => this.passListeners.delete(fn); }

  // --- listening to a note bus --------------------------------------------
  //
  // A note bus is the patch's own signal, *before* it reaches a MIDI output
  // node. Reading it is how the page can play a bus that goes nowhere near a
  // cable - which is most of them while a patch is being built, since a bus
  // only leaves the module if somebody has patched a MIDI out to it.
  //
  // The queue is read exactly once per pass, right after `emu_pass()`: the
  // buses are double-buffered and `pass()` swaps once, so at that moment the
  // front buffer holds precisely what this pass wrote - every event once, none
  // twice. Anything reading per animation frame instead would see one pass in
  // sixteen and drop the rest on the floor.
  onNoteBus(fn) { this.noteBusListeners.add(fn); return () => this.noteBusListeners.delete(fn); }

  // The note a bus is rooted on: the last note-on it carried, or NO_NOTE.
  // Only a watched bus is read at all (App.js watches every bus the patch
  // writes), so a bus nobody is listening to answers NO_NOTE rather than
  // something stale.
  busNote(bus) {
    return bus >= 0 && bus < this.busNotes.length ? this.busNotes[bus] : NO_NOTE;
  }

  watchNoteBus(bus) {
    this.noteBusWatch.set(bus, (this.noteBusWatch.get(bus) ?? 0) + 1);
  }

  unwatchNoteBus(bus) {
    const held = this.noteBusWatch.get(bus);
    if (!held) return;
    if (held > 1) { this.noteBusWatch.set(bus, held - 1); return; }
    this.noteBusWatch.delete(bus);
    // Nobody is reading it any more, which in the patch means nothing writes
    // it: the note it was rooted on is history, not the answer to the next
    // question about it.
    this.busNotes[bus] = NO_NOTE;
  }

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
    const cv = this.column.cv;
    for (let b = 0; b < P.N_CV_BUS; b++) cv[b] = E.emu_cv(b);

    const a = this.activity;
    a.jackIn |= jackIn; a.jackOut |= jackOut; a.gate |= gate;
    if (green > a.green) a.green = green;
    if (red > a.red) a.red = red;

    // The note buses anything is listening to, drained for this pass.
    for (const bus of this.noteBusWatch.keys()) {
      const count = E.emu_note_count(bus);
      for (let i = 0; i < count; i++) {
        const packed = E.emu_note_event(bus, i);
        const event = {
          t: this.now, bus,
          type: (packed >>> 24) & 0xff,
          channel: (packed >>> 16) & 0xff,
          d1: (packed >>> 8) & 0xff,
          d2: packed & 0xff,
        };
        if (event.type === NOTE_ON && event.d2) this.busNotes[bus] = event.d1;
        this.rollNote('bus', event);
        for (const listener of this.noteBusListeners) listener(event);
      }
    }

    const c = this.column;
    c.jackIn |= jackIn; c.jackOut |= jackOut; c.gate |= gate;
    const t = this.trace;
    if (this.now >= c.until) {
      t.jackIn[t.head] = c.jackIn;
      t.jackOut[t.head] = c.jackOut;
      t.gate[t.head] = c.gate;
      for (let b = 0; b < P.N_CV_BUS; b++) t.cv[b][t.head] = c.cv[b];
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
  //
  // A note is filed under *where it was seen*: played into the module, sent
  // out of it, or on note bus N. The same note usually appears more than once
  // - a sequencer writes it to a bus and a MIDI out node then sends it - and
  // that is the point: the roll shows the same phrase at each point in the
  // chain, so a bus carrying what you did not expect is visible against the
  // one that does.
  rollNote(direction, { t, type, d1, d2, channel, target, bus = null }) {
    if (!this.isNote(type)) return;
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

  // What a CV bus holds this pass, in the bus's own units (`CV_FULL` is full
  // scale, bipolar signals run negative).
  cv(bus) { return this.E.emu_cv(bus); }

  // Where each loaded sequencer is, by node index in the running patch. This
  // is what puts a playhead on the step grids while they are being edited.
  seqKind(node) { return this.E.emu_seq_kind(node); }
  seqLanes(node) { return this.E.emu_seq_lanes(node); }
  seqPosition(node, lane) { return this.E.emu_seq_position(node, lane); }

  // The pattern a sequencer is actually holding, which for the ones that
  // derive it - Euclid's Bjorklund, the random sequencer's draw - is the only
  // place it exists: the parameters say k, n and a rotation, and the steps
  // they come out as are the node's own. A cell is 1/0 for a gate pattern and
  // the stored velocity for a note or MIDI drum cell.
  seqLength(node, lane) { return this.E.emu_seq_length(node, lane); }
  seqCell(node, lane, step) { return this.E.emu_seq_cell(node, lane, step); }
  nodeCount() { return this.E.emu_node_count(); }

  // What a Harmony node would play, read from the node itself: where the
  // key's chords sit, how much the walk wants each move, and the loop it has
  // written down. `harmonyDegrees` is 0 for every other algorithm, so it is
  // also the test for whether there is a circle to draw.
  //
  // The weights are `Harmony::weigh` - the same function the draw reads - so
  // the picture is the firmware's opinion and not a second one beside it.
  // That is why this is here and not arithmetic in the page: a rule the app
  // reimplemented would be a rule that could drift.
  harmonyDegrees(node) { return this.E.emu_harmony_degrees(node); }
  harmonyDegree(node) { return this.E.emu_harmony_degree(node); }
  harmonyPitch(node, degree) { return this.E.emu_harmony_pitch(node, degree); }
  harmonyTriad(node, degree) { return this.E.emu_harmony_triad(node, degree); }
  harmonyWeight(node, from, to) { return this.E.emu_harmony_weight(node, from, to); }
  harmonyLoopLength(node) { return this.E.emu_harmony_loop_length(node); }
  harmonyLoopPosition(node) { return this.E.emu_harmony_loop_position(node); }
  harmonyLoopChord(node, slot) { return this.E.emu_harmony_loop_chord(node, slot); }
}
