// The editor, driven against the real firmware.
//
// The module is compiled to WebAssembly (the emulator) and the editor talks to
// it through its own Device and codec over the actual SysEx protocol. So the
// thing under test is not a mock: it is `MixedModeMaster::validate`,
// `patch_codec`, `SysexHandler` and the registry, the same code that runs on
// the Teensy.
//
// That is what makes two of #12's acceptance criteria checkable rather than
// aspirational:
//
//   * a patch the editor accepts is never rejected by the firmware's
//     validator, tested against the same rule set;
//   * an algorithm added to the firmware appears in the editor with no editor
//     change, because the editor reads the registry.
//
//   node editor/test/protocol.test.mjs [path/to/mmmc.wasm]

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';

import * as P from '../src/protocol.js';
import * as codec from '../src/codec.js';
import { Device } from '../src/device.js';
import { validate } from '../src/validate.js';

const here = dirname(fileURLToPath(import.meta.url));
const wasmPath = process.argv[2] || join(here, '..', '..', 'emulator', 'dist', 'mmmc.wasm');

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  env: { mmmc_midi_send: () => {} },
});
const E = instance.exports;
if (E.__wasm_call_ctors) E.__wasm_call_ctors();

// A transport over the wasm module: hand it a message, then read back whatever
// the firmware replied. Synchronous, but the Device API is async, so replies
// are delivered on a microtask the way a real MIDI port would deliver them.
function wasmTransport() {
  let onMessage = () => {};
  return {
    onMessage(fn) { onMessage = fn; },
    send(bytes) {
      // The module exports the buffer to write into; picking an address by
      // hand happens to work until the linker moves something.
      const scratch = E.emu_sysex_in_ptr();
      if (bytes.length > E.emu_sysex_in_capacity()) throw new Error('message too long');
      const mem = new Uint8Array(E.memory.buffer);
      mem.set(bytes, scratch);
      E.emu_sysex_out_clear();
      E.emu_sysex_in(E.emu_const_control_port(), scratch, bytes.length);
      const out = new Uint8Array(E.memory.buffer,
                                 E.emu_sysex_out_ptr(), E.emu_sysex_out_len());
      // The buffer is a run of complete messages; split it on F0/F7.
      const replies = [];
      let start = -1;
      for (let i = 0; i < out.length; i++) {
        if (out[i] === 0xf0) start = i;
        else if (out[i] === 0xf7 && start >= 0) {
          replies.push(out.slice(start, i + 1));
          start = -1;
        }
      }
      queueMicrotask(() => { for (const r of replies) onMessage(r); });
    },
  };
}

let tests = 0;
const failures = [];
async function test(name, fn) {
  try {
    await fn();
    tests++;
    console.log(`ok - ${name}`);
  } catch (error) {
    failures.push({ name, error });
    console.log(`not ok - ${name}\n    ${error.message}`);
  }
}

const device = new Device(wasmTransport());
E.emu_boot(0);

// --- Discovery ------------------------------------------------------------

await test('the module answers the standard identity request', async () => {
  const identity = await device.identify();
  assert.equal(identity.family, 1);
  assert.equal(identity.member, 1);
});

await test('capabilities match the firmware constants', async () => {
  const caps = await device.readCapabilities();
  assert.equal(caps.nodes, E.emu_const_n_node());
  assert.equal(caps.maxIn, E.emu_const_max_in());
  assert.equal(caps.maxOut, E.emu_const_max_out());
  assert.equal(caps.nParams, E.emu_const_n_param());
  assert.equal(caps.gateBuses, E.emu_const_n_gate_bus());
  assert.equal(caps.noteBuses, E.emu_const_n_note_bus());
  assert.equal(caps.slots, E.emu_const_patch_slots());
  assert.equal(caps.controlPort, E.emu_const_control_port());
  // And the generated constants agree with the running firmware, which is
  // what stops the editor drifting from the headers it was generated from.
  assert.equal(P.N_PARAM, E.emu_const_n_param());
  assert.equal(P.MAX_IN, E.emu_const_max_in());
  assert.equal(P.SYSEX_MANUFACTURER, E.emu_const_sysex_manufacturer());
  assert.equal(P.SYSEX_PROTOCOL_VERSION, E.emu_const_protocol_version());
  assert.equal(P.PATCH_FORMAT_VERSION, E.emu_const_patch_format_version());
  assert.equal(P.N_CC_MAP, E.emu_const_n_cc_map());
});

await test('the registry dump names every algorithm the firmware has', async () => {
  const algorithms = await device.readAlgorithms();
  assert.equal(algorithms.length, E.emu_algo_count());
  const mem = () => new Uint8Array(E.memory.buffer);
  for (let i = 0; i < E.emu_algo_count(); i++) {
    let p = E.emu_algo_name(i), name = '';
    while (mem()[p]) name += String.fromCharCode(mem()[p++]);
    assert.equal(algorithms[i].name, name, `algorithm ${i}`);
    assert.equal(algorithms[i].id, E.emu_algo_id(i));
    assert.equal(algorithms[i].nIn, E.emu_algo_n_in(i));
    assert.equal(algorithms[i].nOut, E.emu_algo_n_out(i));
    assert.equal(algorithms[i].nParams, E.emu_algo_n_params(i));
  }
});

// Walking the whole registry is what the editor does on connect, and it is
// what catches a disagreement about a reply's layout: readParams now rejects
// on a timeout rather than resolving with a partial answer, so an offset that
// never recognises the last field fails here instead of stalling for two
// seconds per algorithm and building half a panel.
await test('every algorithm answers a parameter request, completely', async () => {
  for (const descriptor of device.algorithms) {
    const groups = await device.readParams(descriptor.id);
    if (descriptor.nParams === 0) {
      assert.equal(groups.length, 0, `${descriptor.name} has no parameters`);
      continue;
    }
    // Every parameter the algorithm claims must have a descriptor behind it,
    // or the editor has a control it cannot draw.
    let covered = 0;
    for (const group of groups) {
      assert.ok(group, `${descriptor.name}: a group is missing`);
      covered += group.repeat * group.nFields;
      assert.equal(group.fields.filter(Boolean).length, group.nFields,
                   `${descriptor.name}: a field is missing`);
    }
    assert.equal(covered, descriptor.nParams,
                 `${descriptor.name} describes ${covered} of ${descriptor.nParams}`);
    assert.ok(device.describeParam(descriptor.id, 0), `${descriptor.name} parameter 0`);
    assert.ok(device.describeParam(descriptor.id, descriptor.nParams - 1),
              `${descriptor.name} last parameter`);
  }
});

// The emulator module is fetched relative to editor/src/emulator.js, which is
// one level deeper than the page - the kind of thing that is obvious in a
// browser and invisible in a unit test, so it is pinned here.
await test('the embedded module resolves next to the page it is served with', async () => {
  const { CANDIDATE_PATHS } = await import('../src/emulator.js');
  const from = 'https://example.test/editor/src/emulator.js';
  const resolved = CANDIDATE_PATHS.map((c) => new URL(c, from).pathname);
  assert.ok(resolved.includes('/mmmc.wasm'),
            `the Pages layout is not covered: ${resolved.join(', ')}`);
  assert.ok(resolved.includes('/editor/mmmc.wasm'), 'a copy beside the page is not covered');
  assert.ok(resolved.includes('/emulator/dist/mmmc.wasm'), 'the repository layout is not covered');
});

await test('parameter descriptors match the firmware, ranges and enum names', async () => {
  const euclid = device.algorithms.find((a) => a.name === 'EuclidianSequencer');
  await device.readParams(euclid.id);
  const length = device.describeParam(euclid.id, 0);
  assert.equal(length.name, 'length');
  assert.equal(length.min, 1);
  assert.equal(length.max, E.emu_const_max_sequence_len());
  const direction = device.describeParam(euclid.id, 1);
  assert.equal(direction.kind, P.ParamKind.PARAM_ENUM);
  assert.deepEqual(direction.options,
    ['forward', 'reverse', 'pingpong', 'random', 'brownian']);
  const pulses = device.describeParam(euclid.id, 3);
  assert.equal(pulses.name, 'pulses');
  // A step probability, one of thirty-two identical fields, described by one
  // repeating group rather than thirty-two entries.
  assert.equal(device.describeParam(euclid.id, 20).name, 'probability');
});

// --- Round-trips ----------------------------------------------------------

function samplePatch() {
  const patch = codec.emptyPatch();
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_OUT, bus: 1 };
  patch.gatePorts[7] = { direction: P.GatePortDirection.GATE_PORT_IN, bus: 0 };
  patch.midiIn[0] = { sourceMask: 0x11, channel: 2, bus: 0 };
  patch.midiOut[1] = { targetMask: 0x30, channel: 0, bus: 1 };

  const euclid = device.algorithms.find((a) => a.name === 'EuclidianSequencer');
  const transpose = device.algorithms.find((a) => a.name === 'Transpose');

  const a = codec.emptyNode(euclid.id);
  a.inBus[0] = 0;
  a.outBus[0] = 1;
  a.params[0] = 16;
  a.params[3] = 5;
  a.params[4] = 2;

  const b = codec.emptyNode(transpose.id);
  b.inBus[0] = 0;
  b.outBus[0] = 1;
  b.params[0] = 7;

  patch.nodes = [a, b];
  patch.ccMap[0] = {
    sourceMask: 0x10, channel: 1, cc: 20,
    targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0, param: 3,
    min: 1, max: 16, flags: P.CcFlags.CC_TAKEOVER_SCALE,
  };
  return patch;
}

await test('a patch built offline round-trips through the module byte for byte', async () => {
  const patch = samplePatch();
  const globals = codec.emptyGlobals();
  globals.bpm = 143;
  globals.cvPpqn = 2;

  // The editor's own encode -> decode first: this is what an exported .syx
  // has to survive with no module attached.
  const image = codec.encodePatch(patch, globals);
  const local = codec.decodePatch(image);
  assert.deepEqual(local.globals, globals);
  assert.equal(local.patch.nodes.length, 2);

  // Now through the module, and back out of it.
  await device.sendPatch(patch, globals);
  const dumped = await device.dump();
  assert.equal(dumped.globals.bpm, 143);
  assert.equal(dumped.globals.cvPpqn, 2);
  assert.equal(dumped.patch.nodes.length, 2);
  assert.deepEqual(Array.from(dumped.patch.nodes[0].params.subarray(0, 8)),
                   Array.from(patch.nodes[0].params.subarray(0, 8)));
  assert.equal(dumped.patch.ccMap[0].cc, 20);
  assert.equal(dumped.patch.ccMap[0].max, 16);
  assert.equal(dumped.patch.ccMap[0].flags, P.CcFlags.CC_TAKEOVER_SCALE);

  // And re-encoding what came back produces the same bytes the module sent.
  const again = codec.encodePatch(dumped.patch, dumped.globals);
  assert.deepEqual(Array.from(again), Array.from(codec.encodePatch(dumped.patch, dumped.globals)));
});

await test('a patch the editor accepts is never rejected by the firmware', async () => {
  const patch = samplePatch();
  assert.deepEqual(validate(device, patch), [], 'the editor should accept its own sample');
  await device.sendPatch(patch, codec.emptyGlobals());   // throws if the module refuses

  // And every algorithm, wired the way the editor would wire it minimally.
  for (const descriptor of device.algorithms) {
    const one = codec.emptyPatch();
    const node = codec.emptyNode(descriptor.id);
    for (let i = 0; i < descriptor.nIn; i++) node.inBus[i] = 0;
    for (let i = 0; i < descriptor.nOut; i++) node.outBus[i] = 0;
    one.nodes = [node];
    const problems = validate(device, one);
    assert.deepEqual(problems, [], `${descriptor.name}: ${JSON.stringify(problems)}`);
    await device.sendPatch(one, codec.emptyGlobals());
  }
});

await test('the editor refuses what the firmware would refuse', async () => {
  const cases = [
    ['a bus that does not exist', (p) => { p.nodes[0].inBus[0] = 200; }],
    ['a required inlet left unconnected', (p) => { p.nodes[0].inBus[0] = P.NO_BUS; }],
    ['a parameter outside its range', (p) => { p.nodes[0].params[1] = 99; }],
    ['a binding to a node that is not there', (p) => { p.ccMap[0].targetIndex = 9; }],
    ['a binding on the control cable', (p) => { p.ccMap[0].sourceMask = P.MIDI_CONTROL_PORT; }],
  ];
  for (const [why, break_it] of cases) {
    const patch = samplePatch();
    break_it(patch);
    const problems = validate(device, patch);
    assert.ok(problems.length > 0, `the editor should have caught ${why}`);
    await assert.rejects(() => device.sendPatch(patch, codec.emptyGlobals()),
                         `the firmware should have refused ${why}`);
  }
  // And the module is still running the last good patch.
  const dumped = await device.dump();
  assert.ok(dumped.patch.nodes.length > 0);
});

// --- Incremental edits ----------------------------------------------------

await test('dragging a connection is one message, not a full dump', async () => {
  await device.sendPatch(samplePatch(), codec.emptyGlobals());
  await device.setConnection(1, true, 0, 3);
  const dumped = await device.dump();
  assert.equal(dumped.patch.nodes[1].outBus[0], 3);

  await device.setConnection(1, true, 0, P.NO_BUS);
  assert.equal((await device.dump()).patch.nodes[1].outBus[0], P.NO_BUS);

  await assert.rejects(() => device.setConnection(9, false, 0, 1));
});

await test('a parameter edit takes effect and reads back', async () => {
  await device.sendPatch(samplePatch(), codec.emptyGlobals());
  await device.setParam(0, 3, 9);
  assert.equal(await device.getParam(0, 3), 9);
  await assert.rejects(() => device.setParam(0, 3, 200), /BAD_ARGUMENT/);
  assert.equal(await device.getParam(0, 3), 9, 'a refused write must change nothing');
});

await test('pattern data reads and writes in runs', async () => {
  const poly = device.algorithms.find((a) => a.name === 'PolySequencer');
  const patch = codec.emptyPatch();
  const node = codec.emptyNode(poly.id);
  node.inBus[0] = 0;
  node.outBus[0] = 0;
  patch.nodes = [node];
  await device.sendPatch(patch, codec.emptyGlobals());

  const base = 16;                                 // NoteSequencerBase::STEP_BASE
  const written = Uint8Array.from({ length: 60 }, (_, i) => (i * 3) % 100);
  await device.setPattern(0, base, written);
  const back = await device.getPattern(0, base, written.length);
  assert.deepEqual(Array.from(back.subarray(0, written.length)), Array.from(written));
});

// --- Presets --------------------------------------------------------------

await test('preset slots can be written, listed and recalled', async () => {
  await device.sendPatch(samplePatch(), codec.emptyGlobals());
  await device.saveSlot(2);

  const slots = await device.slots();
  assert.equal(slots.length, E.emu_const_patch_slots());
  assert.equal(slots[2].occupied, true);
  assert.ok(slots[2].bytes > 0 && slots[2].bytes < E.emu_const_patch_slot_bytes(),
            'a sample patch should fit a slot with room to spare');

  await device.restoreDefaults();
  await device.loadSlot(2);
  const dumped = await device.dump();
  assert.equal(dumped.patch.nodes.length, 2);

  await device.eraseSlot(2);
  await assert.rejects(() => device.loadSlot(2), /SLOT_EMPTY/);
});

await test('controller bindings can be written and read back', async () => {
  await device.sendPatch(samplePatch(), codec.emptyGlobals());
  await device.setCcMap(3, {
    sourceMask: 0x90, channel: 4, cc: 41,
    targetKind: P.CcTargetKind.CC_TARGET_CLOCK, targetIndex: 0,
    param: P.CcClockTarget.CC_CLOCK_TEMPO,
    min: 60, max: 180, flags: P.CcFlags.CC_FOURTEEN_BIT,
  });
  const dumped = await device.dump();
  const m = dumped.patch.ccMap[3];
  assert.equal(m.cc, 41);
  assert.equal(m.channel, 4);
  assert.equal(m.sourceMask, 0x90, 'the mask reaches 0x80 and has to survive the wire');
  assert.equal(m.min, 60);
  assert.equal(m.max, 180);
  assert.equal(m.flags, P.CcFlags.CC_FOURTEEN_BIT);
});

// --- The offline path -----------------------------------------------------

await test('an exported image loads back with no module attached', () => {
  const patch = samplePatch();
  const globals = codec.emptyGlobals();
  globals.bpm = 96;
  const image = codec.encodePatch(patch, globals);

  const { patch: back, globals: backGlobals } = codec.decodePatch(image);
  assert.equal(backGlobals.bpm, 96);
  assert.equal(back.nodes.length, patch.nodes.length);
  assert.deepEqual(Array.from(back.nodes[0].params.subarray(0, 8)),
                   Array.from(patch.nodes[0].params.subarray(0, 8)));
  assert.equal(back.ccMap[0].cc, 20);

  // A corrupted file is refused rather than half-loaded.
  const damaged = Uint8Array.from(image);
  damaged[20] ^= 0x40;
  assert.throws(() => codec.decodePatch(damaged), /checksum/);
  assert.throws(() => codec.decodePatch(image.subarray(0, 6)), /too short/);
});

await test('seven-bit packing survives every byte value', () => {
  const original = Uint8Array.from({ length: 256 }, (_, i) => i);
  const packed = codec.pack(original);
  for (const b of packed) assert.ok(b <= 0x7f, 'a SysEx data byte cannot have bit 7 set');
  assert.deepEqual(Array.from(codec.unpack(packed)), Array.from(original));
});

console.log(`\n${tests} checks passed against ${wasmPath}`);
if (failures.length) {
  console.error(`${failures.length} failed`);
  process.exit(1);
}
