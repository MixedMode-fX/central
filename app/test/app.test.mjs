// The parts of the app that are not the protocol: the patch library, the
// module's runtime seam, and the single-file build.
//
// `protocol.test.mjs` checks the app against the firmware's protocol. This
// checks the three things the merge added, and it checks them the same way -
// against the real module where a module is involved:
//
//   * a patch saved in the browser comes back byte for byte, and a full quota
//     says so rather than losing a patch quietly;
//   * an incoming CC takes main.cpp's path through the control plane, which is
//     what makes learn work from a controller plugged into the browser;
//   * a slider does not take a value from a finger that was scrolling past it;
//   * the built page still contains every module, wired up.
//
//   node app/test/app.test.mjs [path/to/mmmc.wasm]

import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';
import assert from 'node:assert/strict';

import * as P from '../src/protocol.js';
import * as codec from '../src/codec.js';
import { Library, toBase64, fromBase64, ago } from '../src/storage.js';
import { fromPatchJson, toPatchJson, SEQ_FAMILY } from '../src/patchjson.js';
import { validate } from '../src/validate.js';
import { SCALES, scaleMaskOf, STEP_DIRECTIONS, METRONOME_DIVISIONS, METRONOME_FEELS } from '../src/names.js';
import { EXAMPLES } from '../src/examples.js';
import { patchSchema, promptText, schemaText, WORKED_EXAMPLE } from '../src/schema.js';
import { EmbeddedModule } from '../src/module.js';
import { slider } from '../src/views.js';
import { Listener, gateHits } from '../src/audio.js';
import { KITS, LANE_NOTES, PIECES, drumSources, hit, pieceOf, voiceSpec } from '../src/drums.js';
import {
  connectNewNode, patchBlocks, connectionsOf, planConnection, planDisconnect, planClear,
  applyWrite, freeBus, waitingBus, planJackDirection, planPortFlip, applyPortFlip,
} from '../src/graph.js';
import { catalogue, filterGroups, optionsOf } from '../src/picker.js';
import { ENDPOINTS } from '../src/canvas.js';
import {
  autoLayout, layoutOf, socketPoint, blockHeight, forgetNode, BLOCK_W, ROW_H,
} from '../src/layout.js';
import { bundle } from '../tools/bundle.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const wasmPath = process.argv[2] || join(here, '..', '..', 'emulator', 'dist', 'mmmc.wasm');

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

// localStorage, as the browser presents it: strings in, strings out, and a
// quota that can refuse.
function fakeStorage({ limit = Infinity } = {}) {
  const map = new Map();
  return {
    getItem: (key) => (map.has(key) ? map.get(key) : null),
    removeItem: (key) => map.delete(key),
    setItem: (key, value) => {
      if (value.length > limit) {
        const error = new Error('quota');
        error.name = 'QuotaExceededError';
        throw error;
      }
      map.set(key, String(value));
    },
  };
}

// --- the library ------------------------------------------------------------

await test('a saved patch comes back as the same image', async () => {
  const library = new Library(fakeStorage());
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(1));
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_OUT, bus: 3 };
  const globals = codec.emptyGlobals();
  globals.bpm = 137;
  const bytes = codec.encodePatch(patch, globals);

  const entry = library.save({ name: 'a patch', bytes, nodes: patch.nodes.length });
  const read = library.get(entry.id);
  assert.deepEqual([...read.bytes], [...bytes], 'the image changed on the way through');
  const back = codec.decodePatch(read.bytes);
  assert.equal(back.globals.bpm, 137);
  assert.equal(back.patch.nodes.length, 1);
  assert.equal(back.patch.gatePorts[0].bus, 3);
});

await test('the library lists, renames, duplicates and deletes', async () => {
  const library = new Library(fakeStorage());
  const bytes = codec.encodePatch(codec.emptyPatch(), codec.emptyGlobals());
  const first = library.save({ name: 'one', bytes, nodes: 0 });
  const second = library.save({ name: 'two', bytes, nodes: 2 });

  // Newest first: the list exists to answer "what was I just working on?".
  assert.deepEqual(library.list().map((e) => e.name), ['two', 'one']);
  // And it carries no images: a list of patches must not drag every patch
  // through the render path.
  assert.ok(library.list().every((e) => e.image === undefined));

  library.rename(first.id, 'renamed');
  assert.ok(library.list().some((e) => e.name === 'renamed'));

  const copy = library.duplicate(second.id);
  assert.equal(copy.name, 'two copy');
  assert.notEqual(copy.id, second.id);
  assert.equal(library.list().length, 3);

  library.remove(second.id);
  assert.equal(library.list().length, 2);
  assert.equal(library.get(second.id), null);

  // Saving with an id overwrites rather than making another entry.
  library.save({ id: first.id, name: 'renamed', bytes, nodes: 1 });
  assert.equal(library.list().length, 2);
});

await test('a full quota is reported, not swallowed', async () => {
  const library = new Library(fakeStorage({ limit: 40 }));
  assert.ok(library.available, 'the probe write fits');
  const bytes = codec.encodePatch(codec.emptyPatch(), codec.emptyGlobals());
  assert.throws(() => library.save({ name: 'too big', bytes, nodes: 0 }),
                /delete a patch, or export it to a file/);
  // The working patch is autosaved on every edit, so it must never throw: a
  // full quota may not be allowed to break editing.
  library.saveWorking({ id: null, name: 'working', bytes });
});

await test('a browser that stores nothing is a message, not a crash', async () => {
  const denied = {
    getItem: () => { throw new Error('denied'); },
    setItem: () => { throw new DOMException('denied', 'SecurityError'); },
    removeItem: () => {},
  };
  const library = new Library(denied);
  assert.equal(library.available, false);
  assert.match(library.reason, /Export a file instead/);
  assert.deepEqual(library.list(), []);
  assert.equal(library.readWorking(), null);
  // Autosave is called on every render, including in this browser.
  library.saveWorking({ id: null, name: 'x', bytes: new Uint8Array([1, 2, 3]) });
});

await test('the working patch survives a reload', async () => {
  const storage = fakeStorage();
  const bytes = codec.encodePatch(codec.emptyPatch(), codec.emptyGlobals());
  new Library(storage).saveWorking({ id: 'p1', name: 'in progress', bytes });
  const restored = new Library(storage).readWorking();
  assert.equal(restored.name, 'in progress');
  assert.equal(restored.id, 'p1');
  assert.deepEqual([...restored.bytes], [...bytes]);
});

await test('base64 survives every byte value, and ago() reads as English', async () => {
  const all = Uint8Array.from({ length: 256 }, (_, i) => i);
  assert.deepEqual([...fromBase64(toBase64(all))], [...all]);
  const now = Date.now();
  assert.equal(ago(now, now), 'just now');
  assert.equal(ago(now - 120000, now), '2 min ago');
  assert.equal(ago(now - 7200000, now), '2 h ago');
});

// --- the module in the page -------------------------------------------------

// The runtime seam, driven the way the page drives it. No browser: the module
// only wants passes and simulated time, and `EmbeddedModule` keeps everything
// that needs a document (fetching the wasm, the animation frame) out of the
// class body.

await test('the module runs, keeps time and lights the LEDs', async () => {
  const { module } = await instantiate();
  // A freshly booted module runs the default patch, whose clock is internal.
  module.clockStart();
  module.advance(1_000_000);                        // one second of simulated time
  const clock = module.clock();
  assert.ok(clock.running, 'the clock is running');
  assert.ok(clock.count > 0, 'subticks are advancing');
  // 120 BPM for a second is two beats, at MASTER_PPQN * CLOCK_SUBTICK per beat.
  const beats = clock.count / (P.MASTER_PPQN * P.CLOCK_SUBTICK);
  assert.ok(beats > 1.5 && beats < 2.5, `two beats in a second at ${clock.bpm} BPM, got ${beats}`);
  // The green LED is the firmware's, not the page's: it flashes because
  // StatusLeds says so, which only happens if emu_control_service is doing
  // what main.cpp's loop does.
  let flashed = false;
  for (let i = 0; i < 200 && !flashed; i++) {
    module.advance(5000);
    if (module.leds().green > 0) flashed = true;
  }
  assert.ok(flashed, 'the green LED never flashed the beat');
});

await test('a jack tap is an edge, not a level', async () => {
  const { module } = await instantiate();
  module.pulseJack(0, 20);
  assert.equal(module.jackInput(0), 1, 'the jack goes high at once');
  module.advance(50_000);
  assert.equal(module.jackInput(0), 0, 'and comes back down on its own');
});

// The whole point of routing a controller into the page rather than around it:
// an incoming CC is offered to the firmware's control plane first, exactly as
// main.cpp's loop offers it, so a bound knob moves a parameter and never
// reaches the graph.
// The lights, the scope and the piano roll all hang off one thing: the module
// samples itself once per pass, and the page reads what has happened since it
// last painted. Everything below is that seam.

// The bug this closes: the gate dots and jack lamps were read by a timer at
// 10 Hz, and a trigger on this machine is high for one or two passes - one or
// two *milliseconds*. Ninety-eight times in a hundred the poll landed while it
// was low, so the lights showed a pattern nobody was playing.
await test('a pulse too short to paint still reaches the lights', async () => {
  const { module } = await instantiate();
  module.takeActivity();                            // start from a clean latch
  module.pulseJack(0, 2);                           // two milliseconds: a trigger
  module.advance(100_000);                          // six animation frames' worth
  assert.equal(module.jackInput(0), 0, 'the pulse is long over');

  const live = module.takeActivity();
  assert.ok(live.jackIn & 1, 'the jack was high during that frame and the page never knew');
  // And the latch is a latch, not a memory: the next frame shows it low again.
  assert.equal(module.takeActivity().jackIn & 1, 0, 'the lamp would stay lit for ever');
});

// The same sampling, kept: what the scope draws is every millisecond of it,
// not what a paint happened to catch.
await test('the scope keeps the pulse the eye missed', async () => {
  const { module } = await instantiate();
  module.pulseJack(1, 3);
  module.advance(200_000);
  const trace = module.trace;
  assert.ok(trace.filled > 100, `only ${trace.filled} columns recorded`);
  let seen = 0;
  for (let i = 0; i < trace.filled; i++) if (trace.jackIn[i] & 2) seen++;
  assert.ok(seen >= 1 && seen <= 10, `the trace holds ${seen} columns of a 3 ms pulse`);
});

// The other bug this closes, and it is a one-liner with a nasty shape: the log
// is a ring of two hundred, so once it fills, its *length* never changes
// again. A view that redraws when the length changes therefore stops for ever
// at event two hundred - and comes back to life on "clear", which is exactly
// what it looked like from outside.
await test('the MIDI log still reports events once the ring is full', async () => {
  const { module } = await instantiate();
  for (let i = 0; i < 260; i++) module.emitMidi(P.MidiPort.mmMIDI_USB_0, 0xb0, i & 127, 1, 1);
  const length = module.midiLog.length;
  const seq = module.midiSeq;
  assert.ok(length < 260, 'the log is meant to be a ring');
  module.emitMidi(P.MidiPort.mmMIDI_USB_0, 0xb0, 7, 1, 1);
  assert.equal(module.midiLog.length, length, 'the length cannot say anything new');
  assert.equal(module.midiSeq, seq + 1, 'the sequence must, or the view freezes');
  assert.equal(module.midiLog.at(-1).d1, 7, 'and the newest event is the one at the end');
});

await test('the piano roll opens a note, closes it, and holds an open one', async () => {
  const { module } = await instantiate();
  const port = P.MidiPort.mmMIDI_USB_0;
  module.deliverMidi(port, 0x90, 1, 60, 100);       // what a key press does
  module.advance(20_000);
  const held = module.notes.find((n) => n.pitch === 60);
  assert.ok(held, 'the note never reached the roll');
  assert.equal(held.end, null, 'a note still down is drawn to the playhead');
  module.deliverMidi(port, 0x80, 1, 60, 0);
  assert.ok(held.end > held.start, 'the note off has to close the bar it opened');

  // And what the module plays lands on the same time line, marked apart from
  // what was played into it.
  module.emitMidi(P.MidiPort.mmMIDI_SERIAL_1, 0x90, 48, 90, 1);
  const sent = module.notes.find((n) => n.pitch === 48);
  assert.equal(sent.direction, 'out');
  assert.equal(held.direction, 'in');
});

// Listening to a note bus, which is what makes an unfinished patch audible: a
// bus only leaves the module once a MIDI out is patched to it, and while a
// patch is being built most of them are not.
await test('a note bus can be listened to, event by event', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const heard = [];
  module.onNoteBus((event) => heard.push(event));

  // MIDI in on USB 1 onto note bus 0, and *nothing* patched to a MIDI output:
  // the module sends not one byte, and the bus carries everything.
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  await device.sendPatch(patch, codec.emptyGlobals());

  const sent = [];
  module.onMidi((event) => sent.push(event));

  // Nobody is watching the bus yet, so nothing is read from it.
  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x90, 1, 60, 100);
  module.advance(5000);
  assert.equal(heard.length, 0, 'a bus nobody asked for is not read');

  module.watchNoteBus(0);
  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x90, 1, 64, 100);
  module.advance(5000);
  const on = heard.find((e) => e.type === 0x90 && e.d1 === 64);
  assert.ok(on, `nothing came off the bus (${heard.length} events)`);
  assert.equal(on.bus, 0, 'a bus event says which bus it is from');
  assert.equal(on.channel, 1);
  assert.equal(on.d2, 100, 'velocity survives the unpacking');
  assert.equal(sent.length, 0, 'nothing is patched to an output, so nothing is sent');

  // Exactly once: the buses are double-buffered and a pass swaps once, so a
  // sampler that ran twice per pass - or once per frame - would double or drop.
  const before = heard.length;
  module.advance(20_000);
  assert.equal(heard.length, before, 'the same event was read again');

  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x80, 1, 64, 0);
  module.advance(5000);
  assert.ok(heard.some((e) => e.type === 0x80 && e.d1 === 64), 'the note off never arrived');

  // And a bus nobody listens to any more stops being read.
  module.unwatchNoteBus(0);
  const quiet = heard.length;
  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x90, 1, 67, 100);
  module.advance(5000);
  assert.equal(heard.length, quiet, 'the bus is still being read with nobody listening');
});

await test('the monitor setup survives a reload', async () => {
  const storage = fakeStorage();
  new Library(storage).saveListen({
    volume: 0.3, clicks: false, clickVolume: 0.1,
    players: [{ source: 'out', bus: 0, wave: 'sawtooth', volume: 1 },
              { source: 'bus', bus: 3, wave: 'square', volume: 0.5 }],
  });
  const back = new Library(storage).readListen();
  assert.equal(back.players.length, 2, 'both players came back');
  assert.deepEqual(back.players[1], { source: 'bus', bus: 3, wave: 'square', volume: 0.5 });
  assert.equal(back.clickVolume, 0.1, 'the clicks keep their own level');
  assert.equal(back.clicks, false);
  // A browser that stores nothing is not a crash, here as everywhere else.
  const none = new Library(null);
  none.saveListen({ volume: 1 });
  assert.equal(none.readListen(), null);
});

await test('a bound CC moves a parameter and is consumed', async () => {
  const { module, E } = await instantiate();
  const device = await connected(module);

  // A node to bind to, sent as a whole patch the way the app does.
  // Something with a parameter and no inlet that must be connected: the patch
  // has to be one the firmware's validator takes.
  const descriptor = device.algorithms.find((a) => a.nParams > 0 && a.minIn === 0);
  const patch = codec.emptyPatch();
  const node = codec.emptyNode(descriptor.id);
  patch.nodes.push(node);
  await device.sendPatch(patch, codec.emptyGlobals());

  const port = P.MidiPort.mmMIDI_USB_0;
  await device.setCcMap(0, {
    sourceMask: port, channel: 1, cc: 74,
    targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0, param: 0,
    min: 0, max: 0, flags: 0,
  });

  const consumed = module.deliverMidi(port, 0xb0, 1, 74, 100);
  assert.equal(consumed, null, 'a bound CC never reaches the graph');
  module.advance(5000);                              // cc_map applies at a pass boundary
  const value = await device.getParam(0, 0);
  assert.ok(value > 0, `the parameter did not move (${value})`);

  // An unbound CC is not consumed: it is offered to the ports, which is what
  // "taken by N ports" on the play tab counts.
  const accepted = module.deliverMidi(port, 0xb0, 1, 75, 100);
  assert.equal(typeof accepted, 'number', 'an unbound CC goes to the graph');
  assert.equal(E.emu_last_error(), 0);
});

await test('what the module plays reaches whoever is listening', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const heard = [];
  module.onMidi((event) => heard.push(event));

  // MIDI in on USB 1 onto a note bus, and that bus out to DIN 1: the smallest
  // patch that makes the module send something.
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 0, bus: 0 };
  await device.sendPatch(patch, codec.emptyGlobals());

  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x90, 1, 60, 100);
  module.advance(5000);
  assert.ok(heard.some((e) => e.type === 0x90 && e.d1 === 60),
            `no note on came out (${heard.length} events)`);
  assert.ok(module.midiLog.length > 0, 'the log the play tab shows is empty');
  assert.equal(heard[0].target, P.MidiPort.mmMIDI_SERIAL_1, 'the target port is carried through');
});

// --- the example patches ----------------------------------------------------

// Every example, into the real firmware. An example that no longer loads is a
// broken front door: it is the first thing a visitor presses.
await test('every example patch is one the firmware accepts', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const names = Object.keys(EXAMPLES);
  assert.ok(names.length > 10, `only ${names.length} examples`);
  for (const name of names) {
    const example = EXAMPLES[name];
    assert.ok(example.about, `${name} says nothing about itself`);
    const { patch, globals } = fromPatchJson(example.patch, device);
    const problems = validate(device, patch);
    assert.deepEqual(problems, [], `${name}: ${problems.map((p) => `${p.where}: ${p.message}`).join('; ')}`);
    await device.sendPatch(patch, globals);          // throws if the module refuses it
    // And it survives the round trip out through the JSON and back, which is
    // what the files tab does to it.
    const again = fromPatchJson(toPatchJson(patch, globals, device), device);
    assert.deepEqual([...codec.encodePatch(again.patch, again.globals)],
                     [...codec.encodePatch(patch, globals)], `${name} does not round-trip`);
  }
});

// The masks are generated from midi/scale.h; this is the check that the names
// beside them still point at what the firmware would play.
await test('the scale names name the firmware\'s scales', async () => {
  const { E } = await instantiate();
  assert.equal(SCALES.length, E.emu_scale_count(), 'a scale has been added or removed');
  for (const scale of SCALES) {
    assert.equal(scaleMaskOf(scale.label), E.emu_scale_mask(scale.value),
                 `${scale.label} (${scale.key}) is not the mask the firmware uses`);
  }
});

// The enums that live in a class body rather than in a generated header are
// spelled out in names.js, so nothing but a test keeps them honest. These are
// the words a patch *file* is written in - "1/8", "triplet", "pingpong" - so a
// division renamed or reordered in the firmware would silently repoint every
// file that names one.
await test('the note values and directions name the firmware\'s own options', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const optionsOf = (algorithm, param) =>
    device.describeParam(device.algorithms.find((d) => d?.name === algorithm).id, param).options;

  assert.deepEqual(optionsOf('Metronome', 0), METRONOME_DIVISIONS,
                   'Metronome::Division moved under names.js');
  assert.deepEqual(optionsOf('Metronome', 1), METRONOME_FEELS,
                   'Metronome::Feel moved under names.js');
  assert.deepEqual(optionsOf('StepSequencer', 1), STEP_DIRECTIONS,
                   'StepEngine::Direction moved under names.js');
});

// A route to the clock reaches something that is not a node, so no block on
// the canvas carries it. It is still in the patch, and a route in the patch
// that appears nowhere is a route nobody can find or remove.
await test('a route with no block to land on is still listed', async () => {
  const { modulationPanel, describeTarget } = await import('../src/midi.js');
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.modMap[0] = {
    bus: 0, targetKind: P.CcTargetKind.CC_TARGET_CLOCK, targetIndex: 0,
    param: P.CcClockTarget.CC_CLOCK_TEMPO, min: 0, max: 0, depth: 255, flags: 0,
  };
  const app = { patch, device };
  assert.match(describeTarget(app, patch.modMap[0]), /clock/);
  assert.ok(withDom(() => modulationPanel(app)),
            'a clock route has nowhere to be drawn, so the table is where it lives');
});

// --- the patch format, as a schema -------------------------------------------
//
// `schema.js` turns what the device reported into a JSON Schema, so that
// something which is not this editor - a language model, a script - can write
// a patch this editor will load. Two things could go wrong with that and only
// one of them is visible: the schema could be *wrong about the firmware*, and
// nothing on the page would show it. So it is checked here against the real
// module, both ways round - every example patch has to pass it, and a patch
// the firmware would refuse has to fail it.
//
// The checker below is deliberately small: it understands exactly the keywords
// `schema.js` emits and throws on a `$ref` that does not resolve, rather than
// pulling in a validator this app does not otherwise need. A keyword added to
// the generator and not to this list would quietly stop being checked, which
// is why the generator's own vocabulary is written out here.
const KEYWORDS = new Set([
  '$schema', '$id', '$ref', '$defs', 'title', 'description', 'type', 'enum', 'const',
  'minimum', 'maximum', 'minItems', 'maxItems', 'maxLength', 'prefixItems', 'items',
  'properties', 'required', 'additionalProperties', 'allOf', 'anyOf', 'oneOf', 'if', 'then',
]);

const isType = (type, value) => ({
  object: value !== null && typeof value === 'object' && !Array.isArray(value),
  array: Array.isArray(value),
  string: typeof value === 'string',
  boolean: typeof value === 'boolean',
  integer: Number.isInteger(value),
  number: typeof value === 'number',
  null: value === null,
}[type] ?? false);

function schemaProblems(schema, value, root = schema, at = 'the patch') {
  if (schema === true || schema === undefined) return [];
  if (schema === false) return [`${at}: is not allowed here`];
  for (const key of Object.keys(schema)) {
    if (!KEYWORDS.has(key)) throw new Error(`${at}: the checker does not know the keyword "${key}"`);
  }
  const found = [];
  const bad = (why) => found.push(`${at}: ${why}`);

  if (schema.$ref) {
    const target = schema.$ref.replace(/^#\//, '').split('/').reduce((o, key) => o?.[key], root);
    if (!target) throw new Error(`${at}: ${schema.$ref} is not in the schema`);
    found.push(...schemaProblems(target, value, root, at));
  }
  const types = schema.type === undefined ? null : [schema.type].flat();
  if (types && !types.some((type) => isType(type, value))) bad(`should be ${types.join(' or ')}`);
  if ('const' in schema && value !== schema.const) bad(`should be ${JSON.stringify(schema.const)}`);
  if (schema.enum && !schema.enum.includes(value)) bad(`is not one of ${schema.enum.join(', ')}`);
  if (typeof value === 'number') {
    if (schema.minimum !== undefined && value < schema.minimum) bad(`${value} is below ${schema.minimum}`);
    if (schema.maximum !== undefined && value > schema.maximum) bad(`${value} is above ${schema.maximum}`);
  }
  if (typeof value === 'string' && schema.maxLength !== undefined && value.length > schema.maxLength) {
    bad(`is longer than ${schema.maxLength}`);
  }
  if (Array.isArray(value)) {
    if (schema.minItems !== undefined && value.length < schema.minItems) bad(`needs ${schema.minItems} entries`);
    if (schema.maxItems !== undefined && value.length > schema.maxItems) bad(`holds at most ${schema.maxItems}`);
    value.forEach((item, i) => {
      const sub = schema.prefixItems?.[i] ?? schema.items;
      if (sub !== undefined) found.push(...schemaProblems(sub, item, root, `${at}[${i}]`));
    });
  }
  if (isType('object', value)) {
    for (const key of schema.required ?? []) if (!(key in value)) bad(`has no "${key}"`);
    for (const [key, item] of Object.entries(value)) {
      const sub = schema.properties?.[key];
      if (sub === undefined) {
        if (schema.properties && schema.additionalProperties === false) bad(`has no property "${key}"`);
        continue;
      }
      found.push(...schemaProblems(sub, item, root, `${at}.${key}`));
    }
  }
  for (const sub of schema.allOf ?? []) found.push(...schemaProblems(sub, value, root, at));
  if (schema.anyOf && !schema.anyOf.some((sub) => !schemaProblems(sub, value, root, at).length)) {
    bad('matches none of the alternatives');
  }
  if (schema.oneOf) {
    const matched = schema.oneOf.filter((sub) => !schemaProblems(sub, value, root, at).length).length;
    if (matched !== 1) bad(`matches ${matched} of the alternatives, not one`);
  }
  if (schema.if && !schemaProblems(schema.if, value, root, at).length) {
    found.push(...schemaProblems(schema.then, value, root, at));
  }
  return found;
}

// The schema is only worth anything if a patch that passes it is a patch that
// runs, and the examples are the patches this repository already asserts the
// firmware accepts. So they are the fixture: anything the schema refuses here
// is the schema being wrong about the module, not the patch being wrong.
await test('every example patch passes the schema read from the module', async () => {
  const { module } = await instantiate();
  const schema = patchSchema(await connected(module));
  for (const [name, example] of Object.entries(EXAMPLES)) {
    const problems = schemaProblems(schema, example.patch);
    assert.deepEqual(problems, [], `${name}: ${problems.join('; ')}`);
  }
});

// And the other direction, which is the half that matters for a patch written
// by something that has only read the schema: each of these is refused by the
// firmware, so each has to be refused here.
await test('the schema refuses what the firmware refuses', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const schema = patchSchema(device);
  const caps = device.capabilities;
  const refuses = (why, patch) => {
    const problems = schemaProblems(schema, patch);
    assert.ok(problems.length, `${why} passed the schema`);
    // The same patch, at the firmware: the schema must not be refusing
    // something the module would have taken.
    let refused = false;
    try {
      const built = fromPatchJson(patch, device);
      refused = validate(device, built.patch).length > 0;
    } catch { refused = true; }
    assert.ok(refused, `${why} is refused by the schema but accepted by the firmware`);
  };

  refuses('an algorithm this firmware does not have',
          { nodes: [{ algo: 'Reverb' }] });
  refuses('a gate bus past the last one',
          { nodes: [{ algo: 'Metronome', out: [caps.gateBuses] }] });
  refuses('a parameter above its range',
          { nodes: [{ algo: 'Metronome', params: [200] }] });
  refuses('a jack the panel does not have',
          { gate_ports: [{ port: caps.jacks + 1, dir: 'out', bus: 0 }] });
  refuses('more nodes than the module holds',
          { nodes: Array.from({ length: caps.nodes + 1 }, () => ({ algo: 'NOT', in: [0], out: [0] })) });
  refuses('a MIDI port that is not on this module',
          { midi_in: [{ sources: ['DIN 9'], channel: 0, bus: 0 }] });

  // A `seq` block is sugar for one algorithm's parameter layout, so offering
  // it to an algorithm that has none is a patch nobody can pack.
  const problems = schemaProblems(schema, { nodes: [{ algo: 'NOT', seq: { length: 4 } }] });
  assert.ok(problems.length, 'a seq block on a logic gate passed the schema');
});

// The names in the schema are the ones on the panel, because a file written
// against it should read like the module - but the loader has always been
// forgiving about case and spacing, and about the firmware's own enum names.
// The examples are written in the canonical spelling now that a schema says
// what canonical is, so this is what keeps the forgiving path covered.
await test('a MIDI port is read however it is spelled', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const mask = (name) => fromPatchJson(
    { midi_in: [{ sources: [name], channel: 0, bus: 0 }] }, device).patch.midiIn[0].sourceMask;
  const canonical = mask('USB host');
  assert.equal(mask('USB Host'), canonical, 'case matters, and it should not');
  assert.equal(mask('usb_host'), canonical, 'spacing matters, and it should not');
  assert.equal(mask('HOST_1'), canonical, 'the firmware\'s own name for the port is not accepted');
  assert.throws(() => mask('DIN 9'), /no MIDI port/, 'a port that does not exist was accepted');
});

// The sugar is a table in `patchjson.js` and a set of branches here, and both
// name algorithms as strings. A rename in the firmware would leave a patch
// file quietly losing its pattern, so the names are checked against the
// registry rather than against each other.
await test('every algorithm the sequencer sugar names is one the module has', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const known = device.algorithms.filter(Boolean).map((d) => d.name);
  for (const name of Object.keys(SEQ_FAMILY)) {
    assert.ok(known.includes(name), `patchjson.js packs a "seq" block for ${name}, which this firmware has not`);
  }
  const schema = patchSchema(device);
  const branches = schema.properties.nodes.items.allOf;
  assert.equal(branches.length, known.length, 'an algorithm is missing its branch in the schema');
  for (const d of device.algorithms.filter(Boolean)) {
    const branch = branches.find((b) => b.if.properties.algo.const === d.name);
    assert.ok(branch, `${d.name} is not in the schema`);
    const takesSeq = branch.then.properties.seq !== false;
    assert.equal(takesSeq, Boolean(SEQ_FAMILY[d.name]),
                 `${d.name}: the schema and patchjson.js disagree about the "seq" block`);
  }
});

// What the page actually hands over is the prompt, not the schema: a schema
// with no worked example and no statement of what the buses are is a wall.
await test('the prompt carries the schema, the example and this module\'s shape', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  assert.ok(EXAMPLES[WORKED_EXAMPLE], `the prompt's worked example "${WORKED_EXAMPLE}" is not in examples.js`);
  const prompt = promptText(device, { patch: null });
  assert.ok(prompt.includes(schemaText(device).trim()), 'the prompt does not carry the schema');
  assert.ok(prompt.includes(EXAMPLES[WORKED_EXAMPLE].about), 'the prompt does not carry the worked example');
  assert.ok(prompt.includes(`${device.capabilities.jacks} jacks`), 'the prompt does not say what this module is');
  // And the patch on screen, when the page is asked for it.
  const mine = JSON.stringify({ nodes: [{ algo: 'NOT', in: [0], out: [1] }] }, null, 2);
  assert.ok(promptText(device, { patch: mine }).includes(mine), 'the prompt drops the patch it was given');
});

// The page itself. There is no browser here, so what is checked is that it
// builds from a device and says the two things a first visit needs: that this
// is where the prompt is, and - with no module - why there is nothing to copy.
await test('the schema tab builds, and says so when there is no module', async () => {
  const { schemaTab } = await import('../src/schema.js');
  const { module } = await instantiate();
  const device = await connected(module);
  const words = (node) => [node?.text ?? '', ...(node?.children ?? []).map(words)].flat().join(' ');

  const empty = withDom(() => schemaTab({ device: null }));
  assert.match(words(empty), /none is attached/, 'a page with no module has to say why it is empty');

  const app = {
    device, schemaWithPatch: true,
    patchJson: () => JSON.stringify({ nodes: [{ algo: 'NOT', in: [0], out: [1] }] }, null, 2),
    isOpen: () => false, setOpen: () => {}, render: () => {},
  };
  const page = withDom(() => schemaTab(app));
  const text = words(page);
  assert.match(text, /copy the prompt/, 'the prompt cannot be copied from this page');
  assert.match(text, new RegExp(`${device.algorithms.filter(Boolean).length} algorithms`),
               'the page does not say how much of the module the schema covers');
});

// --- the slider guard -------------------------------------------------------

// The event sequences below are the ones a phone actually produces - they were
// read off Chromium with touch emulation, on this page, at 390px - which is
// the only way this is testable without a browser in CI. What matters is the
// first one: a finger that lands on a slider and then scrolls the page leaves
// with the parameter unchanged. Everything else is the check that the cure did
// not kill the patient.
await test('a slider ignores a scrolling finger and obeys a deliberate one', async () => {
  const made = () => {
    const seen = [];
    const range = withDom(() => slider(
      { class: 'slider', min: '0', max: '127', value: '1', 'aria-label': 'test' },
      { onInput: (v) => seen.push(`input ${v}`), onCommit: (v) => seen.push(`commit ${v}`) }));
    return { range, seen };
  };

  // 1. A swipe down the page that begins on the slider. The input jumps to the
  //    finger on touchdown, the browser then takes the gesture for scrolling
  //    and cancels the pointer: nothing here is an edit.
  {
    const { range, seen } = made();
    range.fire('pointerdown', { pointerType: 'touch', clientX: 100, clientY: 400 });
    range.value = '9'; range.fire('input');
    range.fire('pointermove', { clientX: 101, clientY: 386 });
    range.fire('pointercancel');
    range.fire('change');
    assert.equal(range.value, '1', 'a scroll moved the slider');
    assert.deepEqual(seen, [], 'a scroll wrote the parameter');
  }

  // 2. A drag along the slider, which is what a slider is for.
  {
    const { range, seen } = made();
    range.fire('pointerdown', { pointerType: 'touch', clientX: 100, clientY: 400 });
    range.value = '9'; range.fire('input');                       // the landing jump, refused
    range.fire('pointermove', { clientX: 122, clientY: 402 });     // claimed: along it
    range.value = '40'; range.fire('input');
    range.fire('pointerup');
    range.fire('change');
    assert.equal(range.value, '40');
    assert.deepEqual(seen, ['input 40', 'commit 40']);
  }

  // 3. A press held still on the slider, then nudged: also deliberate.
  {
    const { range, seen } = made();
    range.fire('pointerdown', { pointerType: 'touch', clientX: 100, clientY: 400 });
    await new Promise((done) => setTimeout(done, 300));
    range.fire('pointermove', { clientX: 104, clientY: 400 });
    range.value = '12'; range.fire('input');
    range.fire('pointerup');
    range.fire('change');
    assert.deepEqual(seen, ['input 12', 'commit 12']);
  }

  // 4. A mouse, and the keyboard: neither is trying to scroll anything.
  {
    const { range, seen } = made();
    range.fire('pointerdown', { pointerType: 'mouse', clientX: 100, clientY: 400 });
    range.value = '77'; range.fire('input');
    range.fire('pointerup');
    range.fire('change');
    assert.deepEqual(seen, ['input 77', 'commit 77']);
  }
  {
    const { range, seen } = made();
    range.value = '5'; range.fire('input'); range.fire('change');
    assert.deepEqual(seen, ['input 5', 'commit 5']);
  }
});

// --- the single-file build --------------------------------------------------

await test('the built page contains every module, with nothing left to import', async () => {
  const code = bundle('app.js');
  for (const name of ['app.js', 'module.js', 'storage.js', 'perform.js', 'controller.js',
                      'audio.js', 'drums.js', 'library.js', 'scope.js', 'views.js', 'midi.js',
                      'schema.js', 'protocol.js']) {
    assert.ok(code.includes(`__define('${name}'`), `${name} is not in the build`);
  }
  // Nothing may be left that a browser would try to fetch: the built page is
  // opened from a file:// URL, where a fetch cannot work.
  const left = code.match(/^\s*(import|export)\s+[^(]/m);
  assert.equal(left, null, `an ${left?.[1]} statement survived: ${left?.[0]?.trim()}`);

  // And it has to parse. A bundler that produces a syntax error produces a
  // blank page, which is the one failure a smoke test must not miss.
  const scratch = mkdtempSync(join(tmpdir(), 'mmmc-bundle-'));
  const file = join(scratch, 'bundle.mjs');
  writeFileSync(file, code);
  execFileSync(process.execPath, ['--check', file]);
});

// --- the patch as blocks and arrows ------------------------------------------
//
// The canvas draws a patch; it does not hold one. So what is checked here is
// the part that could be wrong in a way a screenshot would not show: that the
// arrows are the buses, that a drag between two sockets produces a patch the
// firmware accepts, and that a drag it would refuse is refused before it is
// made rather than after.

// The app's own "add a node": it arrives connected, which is what makes the
// second node of a chain read the first.
function added(device, patch, algorithmId) {
  const descriptor = device.byId.get(algorithmId);
  const node = codec.emptyNode(algorithmId);
  connectNewNode(device, patch, node, descriptor);
  patch.nodes.push(node);
  return node;
}

const idOf = (name, device) => device.algorithms.find((d) => d && d.name === name).id;

// A drag, end to end: plan it from the patch as it is, then write it. This is
// what `App.applyPlan` does, minus the messages.
function dragged(device, patch, from, to) {
  const plan = planConnection(patchBlocks(device, patch), device.capabilities, from, to);
  if (plan.ok) for (const write of plan.writes) applyWrite(patch, write);
  return plan;
}

await test('the arrows are the buses, not a second model of the patch', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  added(device, patch, idOf('ClockDiv', device));
  added(device, patch, idOf('StepSequencer', device));

  const blocks = patchBlocks(device, patch);
  assert.equal(blocks.length, 2);
  assert.equal(blocks[0].title, 'ClockDiv');
  assert.equal(blocks[1].inlets[0].required, true, 'the advance inlet must be connected');

  const arrows = connectionsOf(blocks);
  assert.equal(arrows.length, 1, 'the sequencer reads the divider');
  assert.equal(arrows[0].from.blockId, 'node:0');
  assert.equal(arrows[0].to.blockId, 'node:1');
  assert.equal(arrows[0].bus, patch.nodes[0].outBus[0]);

  // Move the outlet off its bus from a selector, as the inspector does, and
  // the arrow is gone: there was never anything else holding it there.
  patch.nodes[0].outBus[0] = P.NO_BUS;
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0);
});

await test('a jack and a MIDI port are blocks with one socket each', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_IN, bus: 2 };
  patch.gatePorts[1] = { direction: P.GatePortDirection.GATE_PORT_OUT, bus: 2 };
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 1 };
  patch.midiOut[3] = { targetMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 0, bus: 1 };

  const blocks = patchBlocks(device, patch);
  assert.deepEqual(blocks.map((b) => b.id), ['midiIn:0', 'jack:0', 'jack:1', 'midiOut:3'],
                   'what arrives, then what the patch does, then what leaves');
  assert.equal(blocks[1].outlets.length, 1, 'a jack in drives a bus');
  assert.equal(blocks[2].inlets.length, 1, 'a jack out is driven by one');

  const arrows = connectionsOf(blocks);
  assert.equal(arrows.length, 2, 'jack 1 to jack 2, and MIDI in 1 to MIDI out 4');

  // An unused jack is not a block: eight empty boxes around every patch is not
  // a drawing of it.
  patch.gatePorts[1] = { direction: P.GatePortDirection.GATE_PORT_UNUSED, bus: P.NO_BUS };
  assert.equal(patchBlocks(device, patch).length, 3);
});

await test('a drag from an outlet to an inlet is a patch the firmware takes', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));

  const plan = dragged(device, patch,
    { blockId: 'node:0', at: 0, isOutlet: true },
    { blockId: 'node:1', at: 0, isOutlet: false });
  assert.ok(plan.ok, plan.why);
  assert.equal(plan.writes.length, 2, 'neither end was on a bus, so both moved');
  assert.equal(patch.nodes[0].outBus[0], patch.nodes[1].inBus[0]);

  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, codec.emptyGlobals());     // throws if it is refused
});

await test('one outlet, two readers, one bus', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
  patch.nodes.push(codec.emptyNode(idOf('Metronome', device)));

  dragged(device, patch, { blockId: 'node:0', at: 0, isOutlet: true },
                         { blockId: 'node:1', at: 0, isOutlet: false });
  const second = dragged(device, patch, { blockId: 'node:0', at: 0, isOutlet: true },
                                        { blockId: 'node:2', at: 0, isOutlet: false });
  assert.ok(second.ok, second.why);
  assert.equal(second.writes.length, 1, 'the outlet was already on a bus; only the reader moved');
  assert.equal(patch.nodes[1].inBus[0], patch.nodes[2].inBus[0], 'both read the same bus');

  const arrows = connectionsOf(patchBlocks(device, patch));
  assert.equal(arrows.length, 2);
  assert.deepEqual(arrows.map((a) => a.readers), [2, 2], 'both arrows know the bus is shared');
  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, codec.emptyGlobals());
});

await test('a drag the module would refuse is refused before it is made', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));   // gate out
  patch.nodes.push(codec.emptyNode(idOf('Transpose', device)));       // note in

  const crossed = planConnection(patchBlocks(device, patch), device.capabilities,
    { blockId: 'node:0', at: 0, isOutlet: true },
    { blockId: 'node:1', at: 0, isOutlet: false });
  assert.equal(crossed.ok, false);
  assert.match(crossed.why, /gate outlet cannot drive a note inlet/);

  const twoOutlets = planConnection(patchBlocks(device, patch), device.capabilities,
    { blockId: 'node:0', at: 0, isOutlet: true },
    { blockId: 'node:1', at: 0, isOutlet: true });
  assert.equal(twoOutlets.ok, false);
  assert.match(twoOutlets.why, /two outlets/);

  // And neither of them touched the patch.
  assert.equal(patch.nodes[0].outBus[0], P.NO_BUS);
  assert.equal(patch.nodes[1].inBus[0], P.NO_BUS);
});

await test('which end the drag started at does not change what it connects', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const build = () => {
    const patch = codec.emptyPatch();
    patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
    patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
    return patch;
  };
  const outlet = { blockId: 'node:0', at: 0, isOutlet: true };
  const inlet = { blockId: 'node:1', at: 0, isOutlet: false };

  const forwards = build();
  dragged(device, forwards, outlet, inlet);
  const backwards = build();
  dragged(device, backwards, inlet, outlet);
  assert.deepEqual([...codec.encodePatch(backwards, codec.emptyGlobals())],
                   [...codec.encodePatch(forwards, codec.emptyGlobals())]);
});

await test('disconnecting one arrow says what else the inlet stops hearing', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
  patch.nodes.push(codec.emptyNode(idOf('Metronome', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
  // Two sources merged onto one bus, which the bus model allows and a cable
  // would not: both drivers write gate bus 4, the sequencer reads it.
  patch.nodes[0].outBus[0] = 4;
  patch.nodes[1].outBus[0] = 4;
  patch.nodes[2].inBus[0] = 4;

  const blocks = patchBlocks(device, patch);
  const arrows = connectionsOf(blocks);
  assert.equal(arrows.length, 2, 'two writers, one reader');
  const plan = planDisconnect(blocks, arrows[0]);
  assert.ok(plan.ok);
  assert.match(plan.said, /other source was on gate bus 4/);
  for (const write of plan.writes) applyWrite(patch, write);
  assert.equal(patch.nodes[2].inBus[0], P.NO_BUS);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0,
               'the reader came off the bus, so both arrows went with it');

  // And clearing a socket directly is the same edit from the other end.
  patch.nodes[2].inBus[0] = 4;
  const cleared = planClear(patchBlocks(device, patch), { blockId: 'node:0', at: 0, isOutlet: true });
  assert.ok(cleared.ok);
  for (const write of cleared.writes) applyWrite(patch, write);
  assert.equal(patch.nodes[0].outBus[0], P.NO_BUS);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 1, 'the other source is still there');
});

await test('a free bus is one nothing writes, and running out says so', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  for (let i = 0; i < device.capabilities.gateBuses; i++) {
    const node = codec.emptyNode(idOf('ClockDiv', device));
    node.outBus[0] = i;
    patch.nodes.push(node);
  }
  const blocks = patchBlocks(device, patch);
  assert.equal(freeBus(blocks, device.capabilities, 0), null, 'every gate bus is written');

  patch.nodes.push(codec.emptyNode(idOf('Metronome', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
  const plan = planConnection(patchBlocks(device, patch), device.capabilities,
    { blockId: `node:${patch.nodes.length - 2}`, at: 0, isOutlet: true },
    { blockId: `node:${patch.nodes.length - 1}`, at: 0, isOutlet: false });
  assert.equal(plan.ok, false);
  assert.match(plan.why, /every gate bus/);
});

await test('a jack and a MIDI port are never taken off their bus', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  const blocks = patchBlocks(device, patch);
  const arrow = connectionsOf(blocks)[0];

  // The module validates the pair: a port in use and on no bus is a patch it
  // refuses, so there is nothing here to disconnect - the block goes out of
  // use, or it moves to another bus.
  const cut = planDisconnect(blocks, arrow);
  assert.equal(cut.ok, false);
  assert.match(cut.why, /only in the patch while it is on a bus/);
  const cleared = planClear(blocks, { blockId: 'midiIn:0', at: 0, isOutlet: true });
  assert.equal(cleared.ok, false);

  // Which is worth checking against the rule itself rather than against the
  // wording: taking it off the bus is what the firmware would reject.
  patch.midiOut[0].bus = P.NO_BUS;
  assert.notDeepEqual(validate(device, patch), []);
});

await test('a source added after its listener feeds it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  // A MIDI output waiting on note bus 0 that nothing writes - which `advise`
  // flags, and which "add the MIDI input next" should answer.
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  const blocks = patchBlocks(device, patch);
  assert.equal(waitingBus(blocks, 1), 0, 'note bus 0 is read and written by nothing');
  assert.equal(waitingBus(blocks, 0), null, 'and no gate bus is waiting');

  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  assert.equal(waitingBus(patchBlocks(device, patch), 1), null, 'nothing is waiting once it is fed');
  assert.deepEqual(validate(device, patch), []);
});

// --- the patch's edges, and the list you add them from ------------------------

// The add list is thirty algorithms. Grouped by what the *module* says each
// one is, it is six short lists - and an algorithm added to the firmware
// arrives on a shelf with no change here, which is the same promise the
// registry has always made about names and summaries.
await test('the add list is shelved by what the module says each algorithm is', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const groups = catalogue(device.algorithms, ENDPOINTS);

  const labels = groups.map((g) => g.label);
  assert.deepEqual(labels, ['sequencers', 'notes and MIDI', 'clock', 'modulators',
                            'logic', 'utility', 'edges'],
                   'the shelves, in the order the picker shows them');
  assert.equal(optionsOf(groups).length, device.algorithms.length + ENDPOINTS.length,
               'every algorithm is offered exactly once, and so is every edge');

  // A row says what the algorithm is and what it costs, which is what the
  // native <select> could not: its label was the whole of it.
  const shelf = groups.find((g) => g.label === 'sequencers');
  const euclid = shelf.options.find((o) => o.label === 'EuclidianSequencer');
  assert.ok(euclid, 'a sequencer is on the sequencer shelf');
  assert.match(euclid.note, /2 in · 1 out/);
  assert.match(euclid.hint, /Bjorklund/);
  const lfo = optionsOf(groups).find((o) => o.label === 'LFO');
  assert.match(lfo.note, /clocked/, 'an algorithm the clock drives says so');

  // Search is over everything a row shows, plus the shelf it is on - so the
  // word a musician has in mind finds the row whether or not it is the name.
  assert.deepEqual(
    optionsOf(filterGroups(groups, 'euclid')).map((o) => o.label), ['EuclidianSequencer']);
  const logic = optionsOf(filterGroups(groups, 'logic'));
  assert.ok(logic.length >= 7 && logic.every((o) => o.hint), 'a shelf can be searched for by name');
  assert.deepEqual(filterGroups(groups, 'nothing called this'), [],
                   'a search that matches nothing leaves no empty shelves behind');
});

// An algorithm from firmware this app has never heard of still has to be
// offered: the category is appended to the registry record precisely so that
// an unknown one costs a shelf, not an algorithm.
await test('an algorithm whose category this app does not know is still offered', () => {
  const groups = catalogue([
    { id: 200, name: 'Nova', nIn: 1, nOut: 1, category: 99, summary: 'from later firmware' },
    { id: 201, name: 'Ancient', nIn: 1, nOut: 1, category: P.AlgorithmCategory.CATEGORY_NONE,
      summary: 'from firmware with no categories at all' },
  ]);
  assert.deepEqual(groups.map((g) => g.label), ['other']);
  assert.deepEqual(groups[0].options.map((o) => o.label), ['Nova', 'Ancient']);
});

// A jack faces one way or the other, and which way is a setting on the jack -
// not a kind of jack you have to have chosen before you had one. What matters
// is that the bus survives the turn: a jack in on gate bus 3 turned round is
// the way you listen to gate bus 3, and finding that bus again by hand was
// the old selector's whole cost.
await test('a jack turned round keeps its bus, and an unused one lands on a real one', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const caps = device.capabilities;
  const patch = codec.emptyPatch();
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_IN, bus: 3 };

  const turned = planJackDirection(patch, caps, 0, P.GatePortDirection.GATE_PORT_OUT);
  assert.deepEqual(turned, { index: 0, direction: P.GatePortDirection.GATE_PORT_OUT, bus: 3 });

  // Off, and the bus goes with it: a jack in use is one the module validates
  // against its bus count, and an unused one carries NO_BUS.
  const off = planJackDirection(patch, caps, 0, P.GatePortDirection.GATE_PORT_UNUSED);
  assert.equal(off.bus, P.NO_BUS);
  patch.gatePorts[0] = { direction: off.direction, bus: off.bus };
  assert.deepEqual(validate(device, patch), []);

  // And back on: NO_BUS is not a bus, so it lands on the first.
  const on = planJackDirection(patch, caps, 0, P.GatePortDirection.GATE_PORT_IN);
  assert.equal(on.bus, 0);
  patch.gatePorts[0] = { direction: on.direction, bus: on.bus };
  assert.deepEqual(validate(device, patch), [], 'the module takes what the toggle wrote');
});

// A MIDI port's direction is the same setting to the eye and a different thing
// underneath: the module has four inputs and four outputs, so the toggle moves
// the port. What it carries has to travel with it, or "turn it round" quietly
// loses the cables and the channel it was set to.
await test('a MIDI port turned round takes its cables, channel and bus with it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const caps = device.capabilities;
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 5, bus: 2 };

  const plan = planPortFlip(patch, caps, 0, false);
  assert.equal(plan.ok, true);
  assert.deepEqual(plan.to, { index: 0, isOut: true }, 'the first free output takes it');
  applyPortFlip(patch, plan);
  assert.equal(patch.midiIn[0].sourceMask, 0, 'the input it left is unused');
  assert.deepEqual(
    { mask: patch.midiOut[0].targetMask, channel: patch.midiOut[0].channel, bus: patch.midiOut[0].bus },
    { mask: P.MidiPort.mmMIDI_SERIAL_1, channel: 5, bus: 2 });
  assert.deepEqual(validate(device, patch), []);

  // The patch says one MIDI port, pointing the other way - not two.
  const blocks = patchBlocks(device, patch);
  assert.deepEqual(blocks.map((b) => b.id), ['midiOut:0']);

  // And it can refuse: every port on the other side already in use is a
  // failure with a reason, not a silently dropped edit.
  const full = codec.emptyPatch();
  for (let i = 0; i < caps.midiOut; i++) {
    full.midiOut[i] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  }
  full.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, bus: 0 };
  const refused = planPortFlip(full, caps, 0, false);
  assert.equal(refused.ok, false);
  assert.match(refused.why, /every MIDI output port/);
});

// --- where the blocks go -----------------------------------------------------

await test('the layout runs the signal left to right, and a loop does not hang it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  added(device, patch, idOf('ClockDiv', device));
  added(device, patch, idOf('StepSequencer', device));
  added(device, patch, idOf('GateToNote', device));

  const blocks = patchBlocks(device, patch);
  const positions = autoLayout(blocks, connectionsOf(blocks));
  const x = blocks.map((b) => positions.get(b.id).x);
  assert.ok(x[0] < x[1] && x[1] < x[2], `a chain goes rightwards, not ${x}`);

  // A sequencer resetting the divider that advances it is a legal patch - a
  // bus is read and written once a pass - and a layout that walked it looking
  // for the furthest-left source would never come back.
  patch.nodes[0].inBus[0] = patch.nodes[1].outBus[0];
  const looped = patchBlocks(device, patch);
  const round = autoLayout(looped, connectionsOf(looped));
  assert.equal(round.size, looped.length, 'every block still got a position');
});

await test('a socket is inside the block it belongs to', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  added(device, patch, idOf('DrumSeqGate', device));       // eight outlets, two inlets
  const block = patchBlocks(device, patch)[0];
  const at = { x: 0, y: 0 };
  const last = block.outlets[block.outlets.length - 1];
  const point = socketPoint(at, last.at, true);
  assert.equal(point.x, BLOCK_W, 'an outlet is on the right edge');
  assert.ok(point.y < blockHeight(block), 'and inside the block, which is as tall as its rows');
  assert.equal(socketPoint(at, 1, true).y - socketPoint(at, 0, true).y, ROW_H);
  assert.equal(socketPoint(at, 0, false).x, 0, 'an inlet is on the left edge');
});

await test('a block dragged somewhere is remembered beside the patch, not in it', async () => {
  const library = new Library(fakeStorage());
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(12));
  const bytes = codec.encodePatch(patch, codec.emptyGlobals());
  const entry = library.save({ name: 'arranged', bytes, nodes: 1 });

  library.saveLayout(entry.id, { 'node:0': [400, 120] });
  assert.deepEqual(library.layoutFor(entry.id), { 'node:0': [400, 120] });
  // The image is the patch, and a coordinate is not part of one: what a `.syx`
  // file carries and what the module stores are untouched by arranging it.
  assert.deepEqual([...library.get(entry.id).bytes], [...bytes]);

  // An unsaved patch keeps its arrangement under the same name the autosave
  // uses, and the arrangement follows it into the library.
  library.saveLayout('working', { 'node:0': [10, 20] });
  library.moveLayout('working', 'p-new');
  assert.equal(library.layoutFor('working'), null);
  assert.deepEqual(library.layoutFor('p-new'), { 'node:0': [10, 20] });

  library.dropLayout(entry.id);
  assert.equal(library.layoutFor(entry.id), null);

  // A view preference is remembered, and a patch nobody arranged simply gets
  // the automatic layout.
  library.saveView('list');
  assert.equal(library.readCanvas().view, 'list');
  assert.deepEqual(library.layoutFor('p-new'), { 'node:0': [10, 20] }, 'and the layouts survived it');
});

await test('removing a node moves the blocks after it with it', async () => {
  const saved = { 'node:0': [0, 0], 'node:1': [1, 1], 'node:3': [3, 3], 'jack:2': [9, 9] };
  const after = forgetNode(saved, 1);
  assert.deepEqual(after, { 'node:0': [0, 0], 'node:2': [3, 3], 'jack:2': [9, 9] },
                   'node 3 became node 2, node 1 is gone, the jack did not move');
});

await test('a hand-placed block stays where it was put', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  added(device, patch, idOf('ClockDiv', device));
  added(device, patch, idOf('StepSequencer', device));
  const blocks = patchBlocks(device, patch);
  const arrows = connectionsOf(blocks);
  const placed = layoutOf(blocks, arrows, { 'node:1': [777, 333] });
  assert.deepEqual(placed.get('node:1'), { x: 777, y: 333 });
  assert.deepEqual(placed.get('node:0'), autoLayout(blocks, arrows).get('node:0'),
                   'and nothing else moved because of it');
});

// --- the drums ---------------------------------------------------------------
//
// The kits themselves cannot be checked here - "does this sound like a 909"
// is not a question a test answers - so what is checked is everything around
// them: that a kit can play every drum, that the drum a lane means is the one
// the firmware means by it, that a patch's drum sequencers are found with the
// buses they speak on, and that a hit is played once, by the sequencer that
// made it, rather than by whichever player happened to carry it.

await test('the drum a gate lane plays is the one the firmware names', async () => {
  // A `DrumSeqGate` lane carries no note number at all, so the only thing that
  // says which drum it is, is the note the firmware would send for that lane
  // if this were the MIDI variant. Read it out of the firmware rather than
  // trusting the copy in `drums.js`: a lane renamed there and not here would
  // be a snare on the kick's lane, silently, for ever.
  const source = readFileSync(join(here, '..', '..', 'src', 'algorithm', 'sequencer',
                                   'drum_sequencer.cpp'), 'utf8');
  const found = /GM_DEFAULT_NOTE\[DRUM_SEQ_LANES\]\s*=\s*\{([^}]*)\}/.exec(source);
  assert.ok(found, 'the firmware no longer spells its default notes out where this can read them');
  const notes = found[1].split(',').map((n) => Number(n.trim()));
  assert.deepEqual(LANE_NOTES, notes, 'the lane notes in drums.js are not the firmware’s');
  assert.equal(notes.length, P.DRUM_SEQ_LANES);
  // And the map from note to drum has to have an opinion about every one of
  // them, or a default lane plays the tuned fallback instead of a drum.
  for (const note of notes) assert.notEqual(pieceOf(note), 'perc', `note ${note} has no drum`);
});

await test('every kit can play every drum, and no two kits are the same kit', async () => {
  for (const kit of KITS) {
    for (const piece of PIECES) {
      const spec = voiceSpec(kit.id, piece);
      const parts = ['body', 'noise', 'metal', 'click'].filter((part) => spec[part]);
      assert.ok(parts.length, `${kit.id} has nothing to play ${piece} with`);
      for (const part of parts) {
        assert.ok((spec[part].decay ?? 0) > 0, `${kit.id} ${piece}: ${part} never decays`);
      }
    }
  }
  // The kick is the one everybody knows the difference between. If two kits
  // agree about it, one of them is not a kit.
  const kicks = KITS.map((kit) => JSON.stringify(voiceSpec(kit.id, 'kick')));
  assert.equal(new Set(kicks).size, KITS.length, 'two kits have the same kick');
  // An unknown kit is a saved setup from a version that had other kits, or a
  // selector given a value it should not have: it plays rather than throws.
  assert.ok(voiceSpec('no such kit', 'snare').noise, 'an unknown kit has no fallback');
});

await test('a kit builds real audio nodes for every drum in it', async () => {
  // The recipes are data, and data with a typo in it is a kit that throws the
  // first time somebody plays a crash. So play every drum of every kit into a
  // stand-in for Web Audio and insist each one built something and scheduled
  // it in the future rather than in the past.
  const ctx = fakeAudioContext();
  const out = ctx.createGain();
  for (const kit of KITS) {
    for (const piece of PIECES) {
      const before = ctx.made.length;
      hit(ctx, out, { kit: kit.id, piece, note: 60, velocity: 100, at: 1 });
      assert.ok(ctx.made.length > before, `${kit.id} ${piece} made no sound`);
    }
  }
  for (const node of ctx.made) {
    if (node.startedAt === null) continue;
    assert.ok(node.stoppedAt > node.startedAt, 'a node was stopped before it started');
  }
});

await test('the drum sequencers of a patch are found, with the buses they speak on', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const midi = added(device, patch, idOf('DrumSeqMidi', device));
  const gate = added(device, patch, idOf('DrumSeqGate', device));
  // Three lanes out, five left alone - the ordinary shape of a drum patch.
  gate.outBus[0] = 4; gate.outBus[1] = 5; gate.outBus[2] = 6;

  const sources = drumSources(device, patch);
  assert.equal(sources.length, 2, 'both drum sequencers should be found');
  assert.deepEqual(sources.map((s) => s.key), ['node:0', 'node:1'],
                   'a drum voice is keyed by node index, like everything else addressed');
  assert.equal(sources[0].kind, 'note');
  assert.equal(sources[0].bus, midi.outBus[0], 'the MIDI variant speaks on its note outlet');
  assert.equal(sources[1].kind, 'gate');
  assert.deepEqual(sources[1].lanes.map((lane) => [lane.bus, lane.piece]),
                   [[4, 'kick'], [5, 'snare'], [6, 'hatClosed']],
                   'a gate lane is the drum the firmware means by that lane');
  assert.equal(sources[1].lanes.length, 3, 'a lane on no bus is not a lane to listen to');

  // A patch with no drums in it has no drum voices to show, and nothing else
  // in the registry may be mistaken for one.
  added(device, patch, idOf('ClockDiv', device));
  assert.equal(drumSources(device, patch).length, 2);
});

await test('a drum note is played by its own sequencer, not by the player that carried it', async () => {
  const { listener, module } = await listening();
  const kick = { t: 0, bus: 3, type: 0x90, channel: 10, d1: 36, d2: 100 };

  // Nothing knows about the sequencer yet, so a drum leaving the module is
  // still a drum: it lands on the voice for everything on channel 10 that no
  // sequencer here explains, and never on the player carrying it.
  listener.midi({ ...kick, bus: undefined, target: 1 });
  assert.equal(listener.drums.get('other').hits, 1);
  assert.equal(listener.players[0].voices.size, 0);

  // On a bus, though, it is only heard where something is listening: every
  // bus a patch writes is read for the piano roll, and a page that made a
  // noise for each one would be playing patches nobody asked to hear.
  listener.noteBus(kick);
  assert.equal(listener.drums.get('other').hits, 1, 'a bus nobody listens to stays silent');

  listener.setDrumSources([
    { key: 'node:0', index: 0, label: 'DrumSeqMidi 0', kind: 'note', bus: 3, lanes: [] },
  ]);
  assert.equal(module.watches.get(3), 1, 'a drum sequencer’s bus is read without a player on it');

  listener.noteBus(kick);
  assert.equal(listener.drums.get('node:0').hits, 1, 'the sequencer that wrote the bus plays it');
  assert.equal(listener.drums.get('other').hits, 1, 'and the catch-all does not play it as well');
  assert.equal(listener.players[0].voices.size, 0, 'no player may sound a drum note');

  // Turned off, it is silent - and still not handed to a player.
  listener.drums.get('node:0').setOn(false);
  listener.noteBus(kick);
  assert.equal(listener.drums.get('node:0').hits, 1);
  assert.equal(listener.players[0].voices.size, 0);

  // A note that is not a drum still goes where it always went.
  listener.midi({ t: 0, type: 0x90, channel: 1, d1: 60, d2: 100, target: 1 });
  assert.equal(listener.players[0].voices.size, 1, 'the player still plays what is not a drum');
});

await test('a hit already heard on its bus is not heard again off the cable', async () => {
  const { listener } = await listening();
  listener.setDrumSources([
    { key: 'node:0', index: 0, label: 'DrumSeqMidi 0', kind: 'note', bus: 3, lanes: [] },
  ]);
  // The app says which MIDI targets carry a drum sequencer this page is
  // already listening to on its own bus.
  listener.setDrumOutMask(0b0010);

  listener.noteBus({ t: 0, bus: 3, type: 0x90, channel: 10, d1: 36, d2: 100 });
  listener.midi({ t: 0, type: 0x90, channel: 10, d1: 36, d2: 100, target: 0b0010, bus: undefined });
  assert.equal(listener.drums.get('node:0').hits, 1, 'one hit, not a flam');
  assert.equal(listener.drums.get('other').hits, 0, 'and not a second copy on the catch-all');
  assert.equal(listener.players[0].voices.size, 0, 'the dropped copy is dropped, not played as a note');

  // A drum on a port no drum sequencer here feeds is a real hit, and is heard.
  listener.midi({ t: 0, type: 0x90, channel: 10, d1: 38, d2: 100, target: 0b0100 });
  assert.equal(listener.drums.get('other').hits, 1);
});

await test('a gate lane plays its drum on the edge, not while the gate is high', async () => {
  const { listener, module } = await listening();
  listener.setDrumSources([
    { key: 'node:1', index: 1, label: 'DrumSeqGate 1', kind: 'gate', bus: P.NO_BUS,
      lanes: [{ lane: 0, bus: 4, piece: 'kick' }, { lane: 1, bus: 5, piece: 'snare' }] },
  ]);
  const voice = listener.drums.get('node:1');

  module.levels.gate = 1 << 4;
  module.pass();
  assert.equal(voice.hits, 1, 'the rising edge is the hit');
  module.pass();
  assert.equal(voice.hits, 1, 'a gate that is still high is not a second hit');
  module.levels.gate = 0;
  module.pass();
  module.levels.gate = (1 << 4) | (1 << 5);
  module.pass();
  assert.equal(voice.hits, 3, 'two lanes rising together are two drums');

  // A gate bus no lane is on is not a drum, whatever else is listening to it.
  module.levels.gate = 1 << 9;
  module.pass();
  assert.equal(voice.hits, 3);
});

await test('the gate listener hears what it was pointed at, and each edge once', async () => {
  const none = { jacksOut: 0, jacks: 0, buses: 0 };
  assert.deepEqual(gateHits([{ kind: 'jacks' }], none), []);

  // What it has always done: every output jack, pitched by jack number.
  assert.deepEqual(gateHits([{ kind: 'jacks' }], { ...none, jacksOut: 0b101, jacks: 0b101 }),
                   [{ kind: 'jack', index: 0 }, { kind: 'jack', index: 2 }]);

  // An input jack is not an output jack, and "every output jack" does not
  // hear the one being tapped from the jacks panel. Naming it does.
  assert.deepEqual(gateHits([{ kind: 'jacks' }], { ...none, jacks: 0b1000 }), []);
  assert.deepEqual(gateHits([{ kind: 'jack', index: 3 }], { ...none, jacks: 0b1000 }),
                   [{ kind: 'jack', index: 3 }]);

  // The inside of the module: a gate bus, whether or not anything is patched
  // to it. This is the whole point of the selection.
  assert.deepEqual(gateHits([{ kind: 'bus', index: 7 }], { ...none, buses: 1 << 7 }),
                   [{ kind: 'bus', index: 7 }]);
  assert.deepEqual(gateHits([{ kind: 'bus', index: 7 }], { ...none, buses: 1 << 6 }), []);

  // Both kinds at once, which is what "did it make it out of the module" is
  // asked with - and one blip per thing that fired, however many rows asked.
  assert.deepEqual(
    gateHits([{ kind: 'jacks' }, { kind: 'jack', index: 0 }, { kind: 'bus', index: 2 }],
             { jacksOut: 0b1, jacks: 0b1, buses: 1 << 2 }),
    [{ kind: 'jack', index: 0 }, { kind: 'bus', index: 2 }]);
});

await test('what is being listened to survives a reload', async () => {
  const { listener } = await listening();
  listener.setDrumSources([
    { key: 'node:0', index: 0, label: 'DrumSeqMidi 0', kind: 'note', bus: 3, lanes: [] },
  ]);
  listener.drums.get('node:0').setKit('808');
  listener.drums.get('node:0').setVolume(0.25);
  listener.drums.get('other').setOn(false);
  listener.gateSources = [];
  listener.addGateSource({ kind: 'bus', index: 5 });
  listener.addGateSource({ kind: 'jack', index: 2 });
  listener.addPlayer({ source: 'bus', bus: 1, wave: 'square', volume: 0.5 });

  const saved = JSON.parse(JSON.stringify(listener.toJSON()));
  const { listener: back } = await listening();
  back.restore(saved);

  assert.deepEqual(back.gateSources.map((s) => [s.kind, s.index]), [['bus', 5], ['jack', 2]]);
  assert.equal(back.drums.get('node:0').kit, '808');
  assert.equal(back.drums.get('node:0').volume, 0.25);
  assert.equal(back.drums.get('other').on, false);
  assert.deepEqual(back.players.map((p) => [p.source, p.bus, p.wave]),
                   [['out', 0, 'sawtooth'], ['bus', 1, 'square']]);

  // A node removed renumbers the ones after it, and the kit chosen for a drum
  // sequencer has to move with it rather than stay at the index.
  back.drums.get('node:0').setKit('acoustic');
  back.drumVoice('node:2', { label: 'DrumSeqGate 2' }).setKit('808');
  back.forgetDrumNode(0);
  assert.equal(back.drums.has('node:0'), false, 'the removed node takes its voice with it');
  assert.equal(back.drums.get('node:1').kit, '808', 'the kit moved down with its node');
  assert.equal(back.drums.get('node:1').key, 'node:1', 'and knows its own new key');
  assert.equal(back.drums.has('node:2'), false);

  // A setup saved before any of this existed keeps working, on the defaults.
  const { listener: old } = await listening();
  old.restore({ volume: 0.3, clicks: true, clickVolume: 0.2, players: [{ source: 'out' }] });
  assert.deepEqual(old.gateSources.map((s) => s.kind), ['jacks'], 'the gate listener keeps its default');
  assert.equal(old.drums.get('other').kit, back.drums.get('other').kit);
});

// --- the harness ------------------------------------------------------------

// Enough of a document for `el()` to build an element and for a test to fire
// events at it. views.js touches nothing else, and a fake this small is
// honest: the sequences fired at it come from a real browser.
function fakeDocument() {
  return {
    // `el()` wraps a bare string child in a text node.
    createTextNode(text) { return { nodeType: 3, text: String(text) }; },
    createElement(tag) {
      const listeners = new Map();
      return {
        // Children are kept rather than dropped, so a test can read the words
        // a panel put on the page. Nothing here lays anything out.
        tag, nodeType: 1, className: '', attrs: {}, value: '', children: [],
        setAttribute(key, value) { this.attrs[key] = String(value); if (key === 'value') this.value = String(value); },
        addEventListener(type, fn) {
          if (!listeners.has(type)) listeners.set(type, []);
          listeners.get(type).push(fn);
        },
        append(...kids) { this.children.push(...kids); },
        fire(type, event = {}) { for (const fn of listeners.get(type) ?? []) fn({ type, ...event }); },
      };
    },
  };
}

// Enough of Web Audio to build a drum with. Every node records when it was
// started and stopped, because "a kit that throws on the crash" and "a hit
// scheduled in the past" are the two ways a recipe goes wrong, and neither
// makes a sound to notice.
function fakeAudioContext() {
  const made = [];
  const param = () => ({
    value: 0,
    setValueAtTime() { return this; },
    linearRampToValueAtTime() { return this; },
    exponentialRampToValueAtTime() { return this; },
    setTargetAtTime() { return this; },
  });
  const node = (kind, extra = {}) => {
    const made_ = {
      kind, startedAt: null, stoppedAt: null,
      connect(to) { return to; },
      disconnect() {},
      start(at = 0) { this.startedAt = at; },
      stop(at = 0) { this.stoppedAt = at; },
      ...extra,
    };
    made.push(made_);
    return made_;
  };
  return {
    made,
    state: 'running',
    currentTime: 0,
    sampleRate: 48000,
    destination: node('destination'),
    createGain: () => node('gain', { gain: param() }),
    createOscillator: () => node('oscillator', { type: 'sine', frequency: param(), detune: param() }),
    createBufferSource: () => node('noise', { buffer: null, loop: false, playbackRate: param() }),
    createBiquadFilter: () => node('filter', { type: 'lowpass', frequency: param(), Q: param() }),
    createBuffer: (channels, length) => ({ getChannelData: () => new Float32Array(length) }),
    resume: async () => {},
    suspend: async () => {},
  };
}

// The module as the listener uses it: somewhere to hang the hooks, the gate
// levels of the current pass, and the note-bus watches, so a test can say "a
// pass happened and this bus was high" without a wasm module in the room.
function fakeModule() {
  const hooks = { midi: [], bus: [], frame: [], pass: [] };
  return {
    now: 0,
    levels: { jackIn: 0, jackOut: 0, gate: 0, green: 0, red: 0 },
    jackSources: Array.from({ length: P.GPIO_N }, () => ({ level: 0, hz: 0, pulseUntil: 0 })),
    modes: new Array(P.GPIO_N).fill(0),
    watches: new Map(),
    jackMode(jack) { return this.modes[jack]; },
    jackOutput(jack) { return (this.levels.jackOut >> jack) & 1; },
    jackInput(jack) { return (this.levels.jackIn >> jack) & 1; },
    onMidi(fn) { hooks.midi.push(fn); },
    onNoteBus(fn) { hooks.bus.push(fn); },
    onFrame(fn) { hooks.frame.push(fn); },
    onPass(fn) { hooks.pass.push(fn); },
    watchNoteBus(bus) { this.watches.set(bus, (this.watches.get(bus) ?? 0) + 1); },
    unwatchNoteBus(bus) {
      const held = this.watches.get(bus);
      if (!held) return;
      if (held <= 1) this.watches.delete(bus); else this.watches.set(bus, held - 1);
    },
    pass() { for (const fn of hooks.pass) fn(this.now); },
    hooks,
  };
}

// A listener with the audio on, over both fakes. `toggle()` is the real one:
// it is where the master gain, the click gain and every voice get wired up,
// and a test that skipped it would be testing a listener no user ever has.
async function listening() {
  const module = fakeModule();
  const ctx = fakeAudioContext();
  const had = globalThis.AudioContext;
  globalThis.AudioContext = function AudioContext() { return ctx; };
  try {
    const listener = new Listener(module);
    // The two ways an event reaches it, as the module delivers them: off the
    // cable, and off a note bus.
    listener.noteBus = (event) => { for (const fn of module.hooks.bus) fn(event); };
    await listener.toggle();
    assert.ok(listener.enabled, 'the fake context should come up running');
    return { listener, module, ctx };
  } finally {
    globalThis.AudioContext = had;
  }
}

// views.js reads `document` when it builds something, not when it loads, so
// the fake only has to stand up for the call itself.
function withDom(fn) {
  const had = globalThis.document;
  globalThis.document = fakeDocument();
  try { return fn(); } finally { globalThis.document = had; }
}

async function instantiate() {
  // `let`, and assigned after the instance exists: the module may send MIDI
  // from inside emu_boot(), before the object wrapping it has been made.
  let made = null;
  const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
    env: { mmmc_midi_send: (target, type, d1, d2, channel) => made?.emitMidi(target, type, d1, d2, channel) },
  });
  if (instance.exports.__wasm_call_ctors) instance.exports.__wasm_call_ctors();
  made = new EmbeddedModule(instance.exports);
  return { module: made, E: instance.exports };
}

// A Device over the embedded module, as the app builds one.
async function connected(module) {
  const { Device } = await import('../src/device.js');
  const device = new Device(module);
  await device.readCapabilities();
  await device.readAlgorithms();
  for (const descriptor of device.algorithms) await device.readParams(descriptor.id);
  return device;
}

console.log(`\n${tests} checks passed against ${wasmPath}`);
if (failures.length) {
  console.error(`${failures.length} failed`);
  process.exit(1);
}
