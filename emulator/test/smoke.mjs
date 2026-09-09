// Drives the WebAssembly build through the scenarios test/test_master covers
// natively, so CI proves the module behaves like the firmware it was built
// from. No browser: Node's WebAssembly is enough.
//
//   node emulator/test/smoke.mjs [path/to/mmmc.wasm]

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const wasmPath = process.argv[2] || join(here, '..', 'dist', 'mmmc.wasm');

const sent = [];
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  env: { mmmc_midi_send: (target, type, d1, d2, channel) => sent.push({ target, type, d1, d2, channel }) },
});
const E = instance.exports;
if (E.__wasm_call_ctors) E.__wasm_call_ctors();

const NOTE_ON = 0x90, NOTE_OFF = 0x80, CC = 0xB0;
const USB_0 = 0x01, USB_1 = 0x02, SERIAL_1 = 0x10, SERIAL_2 = 0x20, HOST_1 = 0x80;
const GATE_IN = 1, GATE_OUT = 2;
const NO_BUS = E.emu_const_no_bus();

const algo = name => {
  const mem = new Uint8Array(E.memory.buffer);
  for (let i = 0; i < E.emu_algo_count(); i++) {
    let p = E.emu_algo_name(i), s = '';
    while (mem[p]) s += String.fromCharCode(mem[p++]);
    if (s === name) return E.emu_algo_id(i);
  }
  throw new Error(`no algorithm ${name}`);
};

let now = 0;
const passes = (n, step = 1000) => { for (let i = 0; i < n; i++) { E.emu_pass(now); now += step; } };
const node = (i, name, in0, out0) => { E.emu_patch_node(i, algo(name)); if (in0 !== undefined) E.emu_patch_node_in(i, 0, in0); if (out0 !== undefined) E.emu_patch_node_out(i, 0, out0); };
let tests = 0;
// Jack levels persist across loads as a patched cable would, so every
// scenario starts from silence like the native tests' fresh FakeGpio.
const test = (name, fn) => {
  sent.length = 0; now = 0;
  E.emu_unload();
  for (let j = 0; j < E.emu_const_gpio_n(); j++) E.emu_jack_set_input(j, 0);
  E.emu_patch_reset();
  fn(); tests++; console.log(`ok - ${name}`);
};

test('registry is reachable and names the algorithms', () => {
  assert.ok(E.emu_algo_count() >= 11);
  assert.equal(algo('NOT'), 1);
  assert.equal(algo('Sustain'), 8);
});

test('gate in -> NOT -> GateToNote -> MIDI out: one message per edge', () => {
  E.emu_patch_gate_port(0, GATE_IN, 0);
  node(0, 'NOT', 0, 1);
  node(1, 'GateToNote', 1, 0);
  E.emu_patch_midi_out(0, USB_0, 0, 0);
  assert.equal(E.emu_load(), 0);
  assert.equal(E.emu_jack_mode(0), 1, 'input pullup');
  E.emu_jack_set_input(0, 0);
  passes(10);
  sent.length = 0;
  E.emu_jack_set_input(0, 1);
  passes(10);
  assert.deepEqual(sent, [{ target: USB_0, type: NOTE_OFF, d1: 60, d2: 0, channel: 1 }]);
});

test('worked example: arpeggio to two outputs an octave apart', () => {
  E.emu_patch_midi_in(0, SERIAL_1, 0, 0);
  E.emu_patch_gate_port(0, GATE_IN, 0);
  E.emu_patch_node(0, algo('Arpeggiator'));
  E.emu_patch_node_in(0, 0, 0); E.emu_patch_node_in(0, 1, 0); E.emu_patch_node_out(0, 0, 1);
  node(1, 'Transpose', 1, 2); E.emu_patch_node_param(1, 0, 12);
  E.emu_patch_midi_out(0, USB_0, 1, 1);
  E.emu_patch_midi_out(1, SERIAL_2, 2, 2);
  assert.equal(E.emu_load(), 0);
  assert.equal(E.emu_deliver_midi(SERIAL_1, NOTE_ON, 1, 60, 100), 1);
  assert.equal(E.emu_deliver_midi(SERIAL_1, NOTE_ON, 1, 64, 100), 1);
  assert.equal(E.emu_deliver_midi(SERIAL_1, NOTE_ON, 1, 67, 100), 1);
  assert.equal(E.emu_deliver_midi(USB_1, NOTE_ON, 1, 72, 100), 0, 'not routed');
  passes(3);
  assert.equal(sent.length, 0);
  E.emu_jack_set_input(0, 1); passes(5);
  E.emu_jack_set_input(0, 0); passes(5);
  assert.deepEqual(sent, [
    { target: USB_0, type: NOTE_ON, d1: 60, d2: 100, channel: 1 },
    { target: SERIAL_2, type: NOTE_ON, d1: 72, d2: 100, channel: 2 },
  ]);
});

test('fan-out: one bus, three readers, one pass', () => {
  E.emu_patch_midi_in(0, USB_0, 0, 3);
  E.emu_patch_midi_out(0, USB_1, 0, 3);
  E.emu_patch_midi_out(1, SERIAL_1, 0, 3);
  E.emu_patch_midi_out(2, HOST_1, 0, 3);
  E.emu_patch_gate_port(0, GATE_IN, 4);
  for (let p = 1; p <= 3; p++) E.emu_patch_gate_port(p, GATE_OUT, 4);
  assert.equal(E.emu_load(), 0);
  E.emu_deliver_midi(USB_0, NOTE_ON, 1, 60, 100);
  E.emu_jack_set_input(0, 1);
  E.emu_pass(0);
  assert.deepEqual(sent.map(m => m.target), [USB_1, SERIAL_1, HOST_1]);
  for (let p = 1; p <= 3; p++) assert.equal(E.emu_jack_output(p), 1);
  assert.equal(E.emu_gate_buses() & (1 << 4), 1 << 4);
});

test('validator rejects a bad patch and keeps the running one', () => {
  E.emu_patch_gate_port(0, GATE_OUT, 0);
  node(0, 'NOT', 0, 0);
  assert.equal(E.emu_load(), 0);
  const before = E.emu_node_count();
  E.emu_patch_reset();
  E.emu_patch_node(0, algo('NOT')); E.emu_patch_node_in(0, 0, 99); E.emu_patch_node_out(0, 0, 0);
  assert.equal(E.emu_load(), 4, 'LOAD_NODE_INVALID');
  assert.equal(E.emu_last_node_error(), 2, 'CONFIG_INLET_OUT_OF_RANGE');
  assert.equal(E.emu_last_node_index(), 0);
  assert.equal(E.emu_node_count(), before);
  E.emu_patch_reset();
  E.emu_patch_gate_port(0, GATE_IN, 99);
  assert.equal(E.emu_load(), 2, 'LOAD_GATE_PORT_BUS_OUT_OF_RANGE');
});

test('self feedback: a NOT on its own bus oscillates every pass', () => {
  E.emu_patch_gate_port(0, GATE_OUT, 0);
  node(0, 'NOT', 0, 0);
  assert.equal(E.emu_load(), 0);
  const seen = [];
  for (let i = 0; i < 6; i++) { E.emu_pass(i); seen.push(E.emu_jack_output(0)); }
  assert.deepEqual(seen, [1, 0, 1, 0, 1, 0]);
});

test('sustain: a debounced pedal yields exactly one CC, the default patch from main.cpp', () => {
  E.emu_patch_gate_port(7, GATE_IN, 0);
  node(0, 'Sustain', 0, 0);
  E.emu_patch_midi_out(0, 0xFF, 0, 0);
  assert.equal(E.emu_load(), 0);
  passes(20);          // settle: first stable level (up) is transmitted once
  sent.length = 0;
  E.emu_jack_set_input(7, 1);
  passes(2); E.emu_jack_set_input(7, 0); passes(2); E.emu_jack_set_input(7, 1);   // bounce
  passes(20);          // 20 ms > DEBOUNCE_US
  assert.deepEqual(sent, [{ target: 0xFF, type: CC, d1: 64, d2: 127, channel: 1 }]);
});

test('master clock: the page as interval timer, ClockDiv /24 pulses jack 1 once per beat at 120 BPM', () => {
  E.emu_patch_gate_port(0, GATE_OUT, 0);
  E.emu_patch_node(0, algo('ClockDiv')); E.emu_patch_node_out(0, 0, 0); E.emu_patch_node_param(0, 1, 24);
  assert.equal(E.emu_load(), 0);
  E.emu_clock_set_bpm(120);
  // teensy_clock.cpp: reprogram on request, else free-run.
  let interval = 0, nextAt = 0, rising = [], last = 0;
  for (let t = 0; t < 2_100_000; t += 100) {
    if (E.emu_clock_take_interval_change()) { interval = E.emu_clock_interval_us(); nextAt = t + interval; }
    while (t >= nextAt) { E.emu_clock_advance(); nextAt += interval; }
    E.emu_pass(t);
    const o = E.emu_jack_output(0);
    if (o && !last) rising.push(t);
    last = o;
  }
  assert.equal(interval, Math.floor(60e6 / (120 * 24 * 24)), 'subtick interval');
  assert.ok(rising.length >= 4 && rising.length <= 5, `beats in 2.1 s: ${rising.length}`);
  const period = rising[2] - rising[1];
  assert.ok(Math.abs(period - 500_000) < 2000, `beat period ${period} µs`);
});

test('unload returns every jack to an input', () => {
  E.emu_patch_gate_port(2, GATE_OUT, 1);
  assert.equal(E.emu_load(), 0);
  assert.equal(E.emu_jack_mode(2), 2);
  E.emu_unload();
  assert.equal(E.emu_jack_mode(2), 1);
});

console.log(`${tests} scenarios passed against ${wasmPath}`);
