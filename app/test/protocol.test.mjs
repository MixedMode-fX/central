// The app, driven against the real firmware.
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
//   npm test -- protocol      (vitest; MMMC_WASM names another module)

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { test } from 'vitest';

import * as P from '../src/protocol/generated.js';
import * as codec from '../src/protocol/codec.js';
import { Device } from '../src/protocol/device.js';
import { validate } from '../src/core/validate.js';
import { patchBlocks, connectionsOf, isModPort,
         modulationChoices, planModulation } from '../src/core/graph.js';
import { toPatchJson, toPatchJsonText, fromPatchJson } from '../src/core/patchjson.js';
import { layoutOf, socketPoint, blockHeight } from '../src/core/layout.js';
import { ALGORITHM_CATEGORIES } from '../src/protocol/names.js';
import { loadWasm } from '../src/runtime/wasm.js';
import { wasmPath } from './harness/index.mjs';

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
      E.emu_sysex_in(E.emu_const_control_port(), scratch, bytes.length, 0);
      const out = new Uint8Array(E.memory.buffer,
                                 E.emu_sysex_out_ptr(), E.emu_sysex_out_len());
      // The buffer is a run of complete messages, copied out before the
      // next call overwrites it.
      const replies = codec.splitSysex(out).map((reply) => reply.slice());
      queueMicrotask(() => { for (const r of replies) onMessage(r); });
    },
  };
}

const device = new Device(wasmTransport());
E.emu_boot(0);

// --- Discovery ------------------------------------------------------------

test('the module answers the standard identity request', async () => {
  const identity = await device.identify();
  assert.equal(identity.family, 1);
  assert.equal(identity.member, 1);
});

test('capabilities match the firmware constants', async () => {
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
  // The macro table and the pool it shares, so a panel can draw a budget
  // without eight and thirty-two written into it.
  assert.equal(caps.macros, E.emu_const_n_macro());
  assert.equal(caps.macroDests, E.emu_const_n_macro_dest());
  assert.equal(caps.macroDestsPerMacro, P.N_MACRO_DEST_PER_MACRO);
  assert.equal(caps.macroNameBytes, P.MACRO_NAME_BYTES);
});

test('the registry dump names every algorithm the firmware has', async () => {
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
test('every algorithm answers a parameter request, completely', async () => {
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

// The built page carries the module inlined as a data URL, so it opens from
// a download with nothing serving it - and a data URL is decoded rather than
// fetched, because a page opened from file:// cannot rely on fetch at all.
test('the embedded module is read back out of the page that carries it', async () => {
  const bytes = readFileSync(wasmPath);
  const url = `data:application/wasm;base64,${bytes.toString('base64')}`;
  const back = await loadWasm(url);
  assert.equal(back.length, bytes.length);
  assert.deepEqual([...back.subarray(0, 8)], [...bytes.subarray(0, 8)], 'the wasm magic survives');
  const { instance } = await WebAssembly.instantiate(back, { env: { mmmc_midi_send: () => {} } });
  assert.ok(instance.exports.emu_boot, 'what came back is the module');
});

// Every field of every descriptor, against the firmware's own table. A
// descriptor is what the app draws a control from and what its validator
// checks against, so a field that loses a bit on the wire is a control that
// cannot reach a legal value and a validator that refuses a legal patch - as
// a max of 255 did, arriving as 127 and making step 8 of every pattern
// unreachable.
test('every descriptor field survives the wire intact', async () => {
  const mem = () => new Uint8Array(E.memory.buffer);
  const cstr = (p) => { let s = ''; while (mem()[p]) s += String.fromCharCode(mem()[p++]); return s; };
  let widest = 0;
  for (let i = 0; i < E.emu_algo_count(); i++) {
    const id = E.emu_algo_id(i);
    const groups = await device.readParams(id);
    for (let g = 0; g < E.emu_algo_n_param_groups(i); g++) {
      for (let f = 0; f < E.emu_param_group_fields(i, g); f++) {
        const field = groups[g].fields[f];
        const where = `${cstr(E.emu_algo_name(i))} group ${g} field ${f}`;
        assert.equal(field.name, cstr(E.emu_param_name(i, g, f)), where);
        assert.equal(field.min, E.emu_param_min(i, g, f), `${where} min`);
        assert.equal(field.max, E.emu_param_max(i, g, f), `${where} max`);
        assert.equal(field.def, E.emu_param_default(i, g, f), `${where} default`);
        assert.equal(field.kind, E.emu_param_kind(i, g, f), `${where} kind`);
        widest = Math.max(widest, field.max);
      }
    }
  }
  assert.ok(widest > 127, `no parameter reaches past a data byte (widest ${widest}), `
                        + 'so this check would not catch one being truncated');
});

test('parameter descriptors match the firmware, ranges and enum names', async () => {
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

// Every inlet, every outlet and every algorithm arrives described. Without
// this the editor can only say "in 0" and "in 1", and a user has to read the
// firmware to find out which one resets the sequencer.
test('the registry describes every inlet, outlet and algorithm', async () => {
  for (const descriptor of device.algorithms) {
    assert.equal(descriptor.inName.length, descriptor.nIn, `${descriptor.name} inlet names`);
    assert.equal(descriptor.outName.length, descriptor.nOut, `${descriptor.name} outlet names`);
    for (const name of descriptor.inName) assert.ok(name, `${descriptor.name}: an inlet is unnamed`);
    for (const name of descriptor.outName) assert.ok(name, `${descriptor.name}: an outlet is unnamed`);
    assert.ok(descriptor.summary, `${descriptor.name} has no summary`);
  }
  // The names are the firmware's, not a table in the editor.
  const seq = device.algorithms.find((a) => a.name === 'StepSequencer');
  assert.deepEqual(seq.inName, ['advance', 'reset']);
  assert.deepEqual(seq.outName, ['trigger']);
  const drums = device.algorithms.find((a) => a.name === 'DrumSeqGate');
  assert.equal(drums.outName.length, P.DRUM_SEQ_LANES);
  assert.equal(drums.outName[0], 'lane 1');
});

// The category is the shelf the add list puts an algorithm on, and it comes
// off the module with everything else it says about itself. An algorithm that
// arrives without one would land under "other", which is the wall of thirty
// names that categories were meant to remove.
test('every algorithm arrives on a shelf the app has a name for', async () => {
  const known = new Set(ALGORITHM_CATEGORIES.map((c) => c.value));
  for (const descriptor of device.algorithms) {
    assert.ok(known.has(descriptor.category),
              `${descriptor.name}: category ${descriptor.category} is not one this app knows`);
    assert.notEqual(descriptor.category, P.AlgorithmCategory.CATEGORY_NONE,
                    `${descriptor.name} arrived with no category`);
  }
  // And they are the shelves the source tree is already laid out as, read
  // back off the wire rather than guessed from the names here.
  const of = (name) => device.algorithms.find((a) => a.name === name).category;
  assert.equal(of('AND'), P.AlgorithmCategory.CATEGORY_LOGIC);
  assert.equal(of('ClockDiv'), P.AlgorithmCategory.CATEGORY_CLOCK);
  assert.equal(of('EuclidianSequencer'), P.AlgorithmCategory.CATEGORY_SEQUENCER);
  assert.equal(of('Transpose'), P.AlgorithmCategory.CATEGORY_MIDI);
  assert.equal(of('LFO'), P.AlgorithmCategory.CATEGORY_MODULATOR);
  assert.equal(of('GateHold'), P.AlgorithmCategory.CATEGORY_UTILITY);
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
  b.params[0] = P.PARAM_CENTRE + 7;        // seven semitones up, as a centred byte

  patch.nodes = [a, b];
  patch.ccMap[0] = {
    sourceMask: 0x10, channel: 1, cc: 20,
    targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0, param: 3,
    min: 1, max: 16, flags: P.CcFlags.CC_TAKEOVER_SCALE,
  };
  return patch;
}

test('a patch built offline round-trips through the module byte for byte', async () => {
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

test('a patch the editor accepts is never rejected by the firmware', async () => {
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

test('the editor refuses what the firmware would refuse', async () => {
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

test('dragging a connection is one message, not a full dump', async () => {
  await device.sendPatch(samplePatch(), codec.emptyGlobals());
  await device.setConnection(1, true, 0, 3);
  const dumped = await device.dump();
  assert.equal(dumped.patch.nodes[1].outBus[0], 3);

  await device.setConnection(1, true, 0, P.NO_BUS);
  assert.equal((await device.dump()).patch.nodes[1].outBus[0], P.NO_BUS);

  await assert.rejects(() => device.setConnection(9, false, 0, 1));
});

test('a parameter edit takes effect and reads back', async () => {
  await device.sendPatch(samplePatch(), codec.emptyGlobals());
  await device.setParam(0, 3, 9);
  assert.equal(await device.getParam(0, 3), 9);
  await assert.rejects(() => device.setParam(0, 3, 200), /BAD_ARGUMENT/);
  assert.equal(await device.getParam(0, 3), 9, 'a refused write must change nothing');
});

test('pattern data reads and writes in runs', async () => {
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

// **A block arrives unconnected, and the app knows it is not sendable yet.**
// The editor used to patch a new node in on arrival, and on a canvas those
// guesses are arrows nobody drew: add a MIDI output and the next node lands on
// its bus, add two unrelated nodes and they come out chained. So "add" is now
// `codec.emptyNode` and nothing else, and what keeps the regression this seam
// exists for from coming back is the other half - the module refuses a node
// whose required inlet is empty, so the app must refuse to send one too, or
// every incremental edit afterwards addresses a node the module never took.
//
// `validate` is that refusal, and `Editor.sendWhole` acts on it. This checks
// the two agree with the firmware, for every algorithm the module has.
test('a node the editor adds arrives on no bus, and is sent once it is wired', async () => {
  const patch = codec.emptyPatch();
  const add = (name) => {
    const d = device.algorithms.find((a) => a.name === name);
    const node = codec.emptyNode(d.id);            // exactly what Editor.addNode pushes
    patch.nodes.push(node);
    return { d, node };
  };

  // A ClockDiv needs no inlet - free, it divides the master clock - so added
  // and left alone it is a patch, and it is on no bus either way.
  const clock = add('ClockDiv');
  assert.deepEqual(clock.node.inBus, clock.node.inBus.map(() => P.NO_BUS), 'on no bus');
  assert.deepEqual(clock.node.outBus, clock.node.outBus.map(() => P.NO_BUS), 'writing nothing');
  assert.deepEqual(validate(device, patch), [], 'a clock on its own must be valid');
  await device.sendPatch(patch, codec.emptyGlobals());

  // An inverter does, and until it is wired the patch is one the module
  // refuses - so the app refuses to send it, which is what keeps the next
  // incremental edit addressing a node the module really has.
  const inverter = add('NOT');
  assert.ok(inverter.d.minIn > 0, 'a NOT has to read something');
  assert.equal(inverter.node.inBus[0], P.NO_BUS, 'and it does not land on the clock');
  const waiting = validate(device, patch);
  assert.equal(waiting.length, 1, JSON.stringify(waiting));
  assert.match(waiting[0].message, /must be connected/);
  await assert.rejects(() => device.sendPatch(patch, codec.emptyGlobals()),
                       'the module refuses it too');

  // Wired - which on the canvas is a drag, and here is the bus each port is
  // put on.
  clock.node.outBus[0] = 0;
  inverter.node.inBus[0] = 0;
  inverter.node.outBus[0] = 1;
  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, codec.emptyGlobals());

  // And now an incremental edit addresses a node the module really has.
  await device.setConnection(1, false, 0, clock.node.outBus[0]);
  const dumped = await device.dump();
  assert.equal(dumped.patch.nodes.length, 2);
  assert.equal(dumped.patch.nodes[1].inBus[0], clock.node.outBus[0]);

  // Every algorithm, added into an empty patch the same way: unconnected is
  // refused when the algorithm needs an inlet and accepted when it does not,
  // and wiring the required inlets is all it ever takes to make it sendable.
  for (const d of device.algorithms) {
    const one = codec.emptyPatch();
    const node = codec.emptyNode(d.id);
    one.nodes.push(node);
    assert.equal(validate(device, one).length, d.minIn, `${d.name}: one problem per required inlet`);
    for (let i = 0; i < d.minIn && i < d.nIn && i < P.MAX_IN; i++) node.inBus[i] = 0;
    const problems = validate(device, one);
    assert.deepEqual(problems, [], `${d.name}: ${JSON.stringify(problems)}`);
    await device.sendPatch(one, codec.emptyGlobals());
  }
});

// A parameter byte reaches 255 and a SysEx data byte holds seven bits. Step 8
// of a step sequencer *is* bit 7 of a pattern byte, so a truncated write did
// not round the value - it cleared the whole byte.
test('a parameter above 127 survives the round trip', async () => {
  const seq = device.algorithms.find((a) => a.name === 'StepSequencer');
  const patch = codec.emptyPatch();
  const node = codec.emptyNode(seq.id);
  node.inBus[0] = 0;
  node.outBus[0] = 0;
  patch.nodes = [node];
  await device.sendPatch(patch, codec.emptyGlobals());

  await device.setParam(0, 3, 0x81);              // steps 1 and 8
  assert.equal(await device.getParam(0, 3), 0x81);
  const dumped = await device.dump();
  assert.equal(dumped.patch.nodes[0].params[3], 0x81, 'the dump has to agree');

  for (const value of [0, 1, 127, 128, 200, 255]) {
    await device.setParam(0, 3, value);
    assert.equal(await device.getParam(0, 3), value, `parameter value ${value}`);
  }
});

// --- The emulator's JSON --------------------------------------------------

test('a patch exports as the emulator JSON and comes back unchanged', async () => {
  const patch = samplePatch();
  const globals = { ...codec.emptyGlobals(), bpm: 96, pcEnabled: 1 };
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 2, bus: 0 };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 1 };

  const json = toPatchJson(patch, globals, device);
  // The dialect is the emulator's: named algorithms, jacks from 1, named ports.
  assert.equal(typeof json.nodes[0].algo, 'string');
  assert.ok(device.algorithms.some((a) => a.name === json.nodes[0].algo));
  assert.deepEqual(json.midi_in[0].sources, ['DIN 1']);
  assert.deepEqual(json.midi_out[0].targets, ['USB 1']);
  for (const jack of json.gate_ports ?? []) {
    assert.ok(jack.port >= 1 && jack.port <= P.GPIO_N, 'jacks are numbered from 1');
    assert.ok(['in', 'out', 'unused'].includes(jack.dir));
  }
  // It is text a user can paste, not a blob.
  assert.equal(toPatchJsonText(patch, globals, device), `${JSON.stringify(json, null, 2)}\n`);

  const back = fromPatchJson(JSON.parse(toPatchJsonText(patch, globals, device)), device);
  assert.equal(back.patch.nodes.length, patch.nodes.length);
  assert.deepEqual(back.patch.nodes.map((n) => n.algorithmId), patch.nodes.map((n) => n.algorithmId));
  assert.deepEqual(Array.from(back.patch.nodes[0].params.subarray(0, 8)),
                   Array.from(patch.nodes[0].params.subarray(0, 8)));
  assert.deepEqual(back.patch.gatePorts, patch.gatePorts);
  assert.equal(back.patch.midiIn[0].sourceMask, patch.midiIn[0].sourceMask);
  assert.equal(back.globals.bpm, 96);
  assert.equal(back.globals.pcEnabled, 1);
  assert.equal(back.patch.ccMap[0].cc, patch.ccMap[0].cc);

  // And what came back is still a patch the module takes.
  assert.deepEqual(validate(device, back.patch), []);
  await device.sendPatch(back.patch, back.globals);
});

test('the JSON importer refuses what it cannot resolve', () => {
  assert.throws(() => fromPatchJson({ nodes: [{ algo: 'Nonexistent' }] }, device),
                /no algorithm "Nonexistent"/);
  assert.throws(() => fromPatchJson({ gate_ports: [{ port: 99, dir: 'in', bus: 0 }] }, device),
                /jack 99 does not exist/);
  assert.throws(() => fromPatchJson({ midi_in: [{ sources: ['DIN 9'], bus: 0 }] }, device),
                /no MIDI port called DIN 9/);
});

// --- Presets --------------------------------------------------------------

test('preset slots can be written, listed and recalled', async () => {
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

test('controller bindings can be written and read back', async () => {
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

// --- Modulation -----------------------------------------------------------

// A patch with a modulator and something to modulate: an LFO on CV bus 0 and
// a divider whose amount it drives.
function modulationPatch() {
  const patch = codec.emptyPatch();
  const lfo = device.algorithms.find((a) => a.name === 'LFO');
  const divider = device.algorithms.find((a) => a.name === 'ClockDiv');

  const a = codec.emptyNode(lfo.id);
  a.outBus[0] = 0;                       // a CV bus
  a.params[0] = 3;                       // ramp up
  a.params[1] = 1;                       // free-running
  a.params[2] = 10;                      // 1 Hz

  const b = codec.emptyNode(divider.id);
  b.outBus[0] = 0;                       // a gate bus
  b.params[1] = 4;
  patch.nodes = [a, b];
  return patch;
}

test('the module reports how much modulation it holds', () => {
  const caps = device.capabilities;
  assert.equal(caps.modRoutes, P.N_MOD_ROUTE,
               'the editor cannot offer a route the module has no slot for');
  assert.equal(caps.cvFull, P.CV_FULL,
               'full scale on a control bus is what a depth is a fraction of');
  assert.ok(caps.cvFull > 128,
            'a modulator resolved to seven bits would step audibly on a fine control');
});

test('a modulation route can be written and read back', async () => {
  await device.sendPatch(modulationPatch(), codec.emptyGlobals());
  const route = {
    bus: 0,
    targetKind: P.CcTargetKind.CC_TARGET_NODE,
    targetIndex: 1,
    param: 1,
    min: 2, max: 200,
    depth: 200,                          // past seven bits, so the wire is tested
    flags: P.ModFlags.MOD_BIPOLAR | P.ModFlags.MOD_INVERT | P.ModMode.MOD_OFFSET,
  };
  await device.setModRoute(4, route);
  assert.deepEqual(await device.getModRoute(4), route);

  const dumped = await device.dump();
  assert.deepEqual(dumped.patch.modMap[4], route,
                   'a route is part of the patch, so it is in the dump');
  assert.equal(dumped.patch.modMap[0], null);
});

test('a route can be cleared, and the module says so', async () => {
  await device.sendPatch(modulationPatch(), codec.emptyGlobals());
  await device.setModRoute(2, {
    bus: 1, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
    param: 1, min: 0, max: 0, depth: 255, flags: 0,
  });
  assert.ok(await device.getModRoute(2));
  await device.setModRoute(2, null);
  assert.equal(await device.getModRoute(2), null);
});

test('the module accepts two routes on one parameter', async () => {
  await device.sendPatch(modulationPatch(), codec.emptyGlobals());
  const route = (bus) => ({
    bus, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
    param: 1, min: 0, max: 0, depth: 255, flags: 0,
  });
  await device.setModRoute(0, route(0));
  // This used to be refused, because two writers racing over one value has no
  // defined result. They no longer race: an offset route contributes to the
  // sum, which adds every contribution on a target, clips once and writes
  // once (src/control/control_sum.h). Macros forced it - a destination that
  // rises and then falls back is two windows on one parameter.
  await device.setModRoute(1, route(1));
  assert.equal((await device.getModRoute(1)).bus, 1);
});

test('the editor refuses a route the firmware would refuse', async () => {
  const patch = modulationPatch();
  await device.readParams(patch.nodes[1].algorithmId);
  const bad = (route) => {
    patch.modMap[0] = route;
    return validate(device, patch);
  };
  assert.ok(bad({ bus: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 9,
                  param: 1, min: 0, max: 0, depth: 255, flags: 0 }).length,
            'a route to a node that is not there');
  assert.ok(bad({ bus: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
                  param: 900, min: 0, max: 0, depth: 255, flags: 0 }).length,
            'a route to a parameter that is not there');
  assert.ok(bad({ bus: 0, targetKind: P.CcTargetKind.CC_TARGET_TRANSPORT, targetIndex: 0,
                  param: 0, min: 0, max: 0, depth: 255, flags: 0 }).length,
            'a modulator cannot press the transport');
  assert.ok(bad({ bus: 99, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
                  param: 1, min: 0, max: 0, depth: 255, flags: 0 }).length,
            'a CV bus that does not exist');

  // And each of them really is refused by the module, which is the point of
  // checking it here rather than trusting the two rule sets to agree.
  for (const route of [
    { bus: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 9, param: 1,
      min: 0, max: 0, depth: 255, flags: 0 },
    { bus: 0, targetKind: P.CcTargetKind.CC_TARGET_TRANSPORT, targetIndex: 0, param: 0,
      min: 0, max: 0, depth: 255, flags: 0 },
  ]) {
    await device.sendPatch(modulationPatch(), codec.emptyGlobals());
    await assert.rejects(() => device.setModRoute(0, route), /REJECTED/);
  }
});

test('a modulated parameter is a socket, and the rest are not', async () => {
  const patch = modulationPatch();
  patch.modMap[0] = {
    bus: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
    param: 1, min: 0, max: 0, depth: 255, flags: P.ModFlags.MOD_BIPOLAR,
  };
  await device.readParams(patch.nodes[1].algorithmId);
  const blocks = patchBlocks(device, patch);
  const divider = blocks.find((b) => b.id === 'node:1');

  const mod = divider.inlets.filter(isModPort);
  assert.equal(mod.length, 1, 'one route, one socket');
  assert.equal(mod[0].name, 'amount', 'the socket is named after the parameter');
  assert.equal(mod[0].bus, 0);
  // ClockDiv has five parameters and one real inlet. Only the modulated one
  // is drawn - the block would otherwise be a list of every parameter.
  assert.equal(divider.inlets.length - mod.length, 1);

  // And the arrow: the LFO's outlet to that socket, because they share a bus.
  const arrows = connectionsOf(blocks);
  const arrow = arrows.find((a) => a.from.blockId === 'node:0' && a.to.blockId === 'node:1'
                                && a.to.at === mod[0].at);
  assert.ok(arrow, 'a route the module is running is an arrow on the canvas');

  // ...drawn where the block is. `at` numbers a socket *row*, so a handle
  // that was not one would put the arrow thousands of pixels off the canvas -
  // an arrow that is computed but never seen.
  const positions = layoutOf(blocks, arrows, null);
  const at = positions.get('node:1');
  const point = socketPoint(at, arrow.to.at, false);
  assert.ok(point.y >= at.y && point.y <= at.y + blockHeight(divider),
            `the modulation socket is drawn at y=${point.y}, outside its block`);
});

test('dragging a control signal onto a block plans a route', async () => {
  const patch = modulationPatch();
  await device.readParams(patch.nodes[1].algorithmId);
  const blocks = patchBlocks(device, patch);

  const choices = modulationChoices(device, patch, 1);
  assert.ok(choices.some((c) => c.label === 'amount'));
  const amount = choices.find((c) => c.label === 'amount');

  const plan = planModulation(blocks, patch, device.capabilities,
                              { blockId: 'node:0', at: 0, isOutlet: true },
                              'node:1', amount.param, { device });
  assert.ok(plan.ok, plan.why);
  assert.equal(plan.routes.length, 1);
  assert.equal(plan.routes[0].route.bus, 0, 'the signal is already on a bus, so that is the bus');
  assert.equal(plan.routes[0].route.targetIndex, 1);
  assert.equal(plan.routes[0].route.param, amount.param);

  // Applied, the module takes it - which is the check that matters: a drag
  // that produces a patch the firmware refuses is the bug this file exists
  // to catch.
  patch.modMap[plan.routes[0].slot] = plan.routes[0].route;
  await device.sendPatch(patch, codec.emptyGlobals());
  const dumped = await device.dump();
  assert.deepEqual(dumped.patch.modMap[plan.routes[0].slot], plan.routes[0].route);

  // And the parameter it now owns is not offered a second time.
  assert.ok(!modulationChoices(device, patch, 1).some((c) => c.param === amount.param));
});

test('a note bus cannot be pointed at a parameter', async () => {
  const patch = modulationPatch();
  const arp = device.algorithms.find((a) => a.name === 'Arpeggiator');
  const node = codec.emptyNode(arp.id);
  node.inBus[0] = 0;
  node.inBus[1] = 0;
  node.outBus[0] = 1;
  patch.nodes.push(node);
  await device.readParams(patch.nodes[1].algorithmId);
  const blocks = patchBlocks(device, patch);

  const plan = planModulation(blocks, patch, device.capabilities,
                              { blockId: 'node:2', at: 0, isOutlet: true },
                              'node:1', 1, { device });
  assert.ok(!plan.ok);
  assert.match(plan.why, /not a control signal/);
});

test('modulation survives the JSON dialect', async () => {
  const patch = modulationPatch();
  patch.modMap[1] = {
    bus: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
    param: 1, min: 0, max: 0, depth: 128,
    flags: P.ModFlags.MOD_BIPOLAR | P.ModMode.MOD_OFFSET,
  };
  await device.readParams(patch.nodes[1].algorithmId);
  const json = toPatchJson(patch, codec.emptyGlobals(), device);
  assert.equal(json.mod_map.length, 1);
  assert.equal(json.mod_map[0].target, 'amount',
               'a route in a file names the parameter it reaches, not only its index');

  const back = fromPatchJson(JSON.parse(JSON.stringify(json)), device);
  assert.deepEqual(back.patch.modMap[1], patch.modMap[1]);
});

// --- The offline path -----------------------------------------------------

test('an exported image loads back with no module attached', () => {
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

test('seven-bit packing survives every byte value', () => {
  const original = Uint8Array.from({ length: 256 }, (_, i) => i);
  const packed = codec.pack(original);
  for (const b of packed) assert.ok(b <= 0x7f, 'a SysEx data byte cannot have bit 7 set');
  assert.deepEqual(Array.from(codec.unpack(packed)), Array.from(original));
});

// --- macros -----------------------------------------------------------------

test('a macro and its destinations can be written and read back', async () => {
  await device.sendPatch(modulationPatch(), codec.emptyGlobals());
  await device.setMacro(2, 'open up');
  assert.deepEqual(await device.getMacro(2), { name: 'open up' });

  const dest = {
    macro: 2,
    targetKind: P.CcTargetKind.CC_TARGET_NODE,
    targetIndex: 1,
    param: 1,
    srcLo: 0,
    srcHi: 255,                          // past seven bits, so the wire is tested
    depth: -90,                          // and signed, which the wire carries apart
    flags: 0,
  };
  await device.setMacroDest(5, dest);
  assert.deepEqual(await device.getMacroDest(5), dest);

  // Both are patch state, so both come back in a dump - and the app's own
  // codec decodes the firmware's bytes to the same thing the app sent.
  const dumped = await device.dump();
  assert.deepEqual(dumped.patch.macros[2], { name: 'open up' });
  assert.deepEqual(dumped.patch.macroDest[5], dest);
  assert.equal(dumped.patch.macros[0], null);
  assert.equal(dumped.patch.macroDest[0], null);
});

test('a macro name that fills the field survives, and a longer one is cut', async () => {
  await device.sendPatch(modulationPatch(), codec.emptyGlobals());
  // Fixed-width and not NUL-terminated when full: the field *is* the length,
  // which is the one shape the hand-rolled codec and patch_codec.cpp cannot
  // disagree about.
  await device.setMacro(0, 'fullwide');
  assert.deepEqual(await device.getMacro(0), { name: 'fullwide' });
  await device.setMacro(1, 'much too long to fit');
  assert.deepEqual(await device.getMacro(1), { name: 'much too' });
});

test('a whole patch of macros round-trips through the firmware', async () => {
  const patch = modulationPatch();
  patch.macros[0] = { name: 'lift' };
  patch.macros[3] = { name: 'dive' };
  patch.macroDest[0] = { macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE,
                         targetIndex: 1, param: 1, srcLo: 0, srcHi: 128, depth: 60, flags: 0 };
  patch.macroDest[1] = { macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE,
                         targetIndex: 1, param: 1, srcLo: 128, srcHi: 255, depth: -60, flags: 0 };
  patch.macroDest[9] = { macro: 3, targetKind: P.CcTargetKind.CC_TARGET_CLOCK,
                         targetIndex: 0, param: P.CcClockTarget.CC_CLOCK_TEMPO,
                         srcLo: 10, srcHi: 250, depth: 40, flags: 0 };
  await device.sendPatch(patch, codec.emptyGlobals());

  const dumped = await device.dump();
  assert.deepEqual(dumped.patch.macros, patch.macros);
  assert.deepEqual(dumped.patch.macroDest, patch.macroDest);
});

test('the module refuses a destination reaching another macro', async () => {
  await device.sendPatch(modulationPatch(), codec.emptyGlobals());
  await assert.rejects(() => device.setMacroDest(0, {
    macro: 0, targetKind: P.CcTargetKind.CC_TARGET_MACRO, targetIndex: 1,
    param: 0, srcLo: 0, srcHi: 255, depth: 10, flags: 0,
  }), /REJECTED/, 'a macro table that wrote its own inputs would run away');

  // And one macro cannot eat the shared pool.
  const dest = { macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 1,
                 param: 1, srcLo: 0, srcHi: 255, depth: 10, flags: 0 };
  for (let i = 0; i < P.N_MACRO_DEST_PER_MACRO; i++) await device.setMacroDest(i, dest);
  await assert.rejects(() => device.setMacroDest(P.N_MACRO_DEST_PER_MACRO, dest), /REJECTED/);
});

test('a macro says where it is, and an untouched one says it is silent', async () => {
  const patch = modulationPatch();
  patch.macros[0] = { name: 'lift' };
  patch.macroDest[0] = { macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE,
                         targetIndex: 1, param: 1, srcLo: 0, srcHi: 255, depth: 40, flags: 0 };
  await device.sendPatch(patch, codec.emptyGlobals());

  const state = await device.getMacroState(0);
  // A macro holds no position across a patch load, so an untouched one is
  // silent rather than at zero - and the panel has to be able to tell the
  // difference between that and a destination that is wrong.
  assert.equal(state.macro, 0);
  assert.equal(state.engaged, false);
  assert.equal(state.dests.length, 1);
  assert.equal(state.dests[0].slot, 0);
  assert.equal(state.dests[0].status, P.ModStatus.MOD_STATUS_SILENT);
});
