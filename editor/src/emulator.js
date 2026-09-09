// The module, embedded in the page.
//
// `Device` talks to a *transport*, not to Web MIDI, so the firmware compiled
// to WebAssembly is a transport like any other: the editor drives it over the
// same SysEx protocol, through the same codec, and cannot tell the difference.
// That is the seam the tests already use to check the editor against the real
// validator; this is the same thing with the browser as the host.
//
// Two reasons it matters more than a convenience:
//
//   * **Web MIDI is not available on iOS at all**, and needs a permission
//     prompt and an OTG cable on Android. An editor that could only reach a
//     module through Web MIDI would be unusable on most phones. The embedded
//     module needs none of it and runs in every browser.
//   * It makes the editor demonstrable and testable with no hardware, which is
//     what the Pages deployment is for.
//
// What it is *not*: a substitute for a module. Presets live in RAM, so a
// reload loses them, and the jacks go nowhere. It is the firmware, running.

import * as P from './protocol.js';

// Where mmmc.wasm might be. These resolve against *this module's* URL
// (editor/src/emulator.js), not the page's, which is one level deeper than it
// looks - the reason to check this in a browser rather than reason about it:
//
//   ../../mmmc.wasm              /mmmc.wasm       the Pages layout
//   ../mmmc.wasm                 /editor/...      a copy beside the page
//   ../../emulator/dist/...      the repository served from its root
export const CANDIDATE_PATHS = ['../../mmmc.wasm', '../mmmc.wasm', '../../emulator/dist/mmmc.wasm'];

// Simulated microseconds advanced per animation frame. The module is a
// gate-rate machine: it wants passes, not wall-clock. One frame of real time
// is one frame of simulated time, capped so a backgrounded tab does not come
// back and run a minute of passes in one go.
const MAX_FRAME_US = 100000;
const PASS_US = 1000;

export class EmulatedModule {
  constructor(exports) {
    this.E = exports;
    this.now = 0;
    this.clockInterval = 0;
    this.clockNextAt = 0;
    this.running = false;
    this.handler = () => {};
    this.E.emu_boot(0);
  }

  static async load(url) {
    const urls = url ? [url] : CANDIDATE_PATHS;
    let lastError = null;
    for (const candidate of urls) {
      try {
        const response = await fetch(new URL(candidate, import.meta.url));
        if (!response.ok) { lastError = new Error(`${candidate}: ${response.status}`); continue; }
        const bytes = await response.arrayBuffer();
        const { instance } = await WebAssembly.instantiate(bytes, {
          // The module sends musical MIDI here. The editor is a control-plane
          // client and has nowhere to put it, so it is counted and dropped;
          // the emulator page is where you go to hear a patch.
          env: { mmmc_midi_send: () => {} },
        });
        if (instance.exports.__wasm_call_ctors) instance.exports.__wasm_call_ctors();
        return new EmulatedModule(instance.exports);
      } catch (error) {
        lastError = error;
      }
    }
    throw new Error(`could not load the emulator module (${lastError?.message ?? 'not found'})`);
  }

  // --- the transport Device wants ---------------------------------------

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

  // --- keeping it alive --------------------------------------------------

  // The module is a machine that runs passes. Without this it would answer
  // SysEx and do nothing else: no beat on the green LED, no debounced
  // autosave, no quantised patch swap ever arriving.
  start() {
    if (this.running) return;
    this.running = true;
    let last = 0;
    const frame = (ts) => {
      if (!this.running) return;
      const dt = last ? Math.min(MAX_FRAME_US, (ts - last) * 1000) : 0;
      last = ts;
      this.advance(dt);
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  stop() { this.running = false; }

  advance(microseconds) {
    const E = this.E;
    const passes = Math.floor(microseconds / PASS_US);
    for (let i = 0; i < passes; i++) {
      // The page is the interval timer, exactly as teensy_clock.cpp is on the
      // module: reprogram when the clock asks, else free-run.
      if (E.emu_clock_take_interval_change()) {
        this.clockInterval = E.emu_clock_interval_us();
        this.clockNextAt = this.now + this.clockInterval;
      }
      while (this.clockInterval && this.now >= this.clockNextAt) {
        E.emu_clock_advance();
        this.clockNextAt += this.clockInterval;
      }
      E.emu_control_service(this.now);
      E.emu_pass(this.now);
      this.now += PASS_US;
    }
  }

  // --- what the editor can show that a MIDI port cannot -------------------

  // The module's entire feedback surface, which a page can show and a MIDI
  // cable cannot.
  leds() {
    return { green: this.E.emu_led(0), red: this.E.emu_led(1) };
  }

  // Which gate buses are high this pass: the live view of a running patch
  // that the module has no way to display.
  gateBuses() { return this.E.emu_gate_buses(); }

  clock() {
    return {
      bpm: this.E.emu_clock_bpm(),
      running: this.E.emu_clock_running() === 1,
      source: this.E.emu_clock_source(),
      count: this.E.emu_clock_count(),
    };
  }

  get name() { return 'built-in emulator'; }
  get deviceId() { return P.SYSEX_DEFAULT_DEVICE; }
}
