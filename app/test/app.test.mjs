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
import { Library, toBase64, fromBase64, ago } from '../src/library.js';
import { fromPatchJson, toPatchJson } from '../src/patchjson.js';
import { validate } from '../src/validate.js';
import { SCALES, scaleMaskOf } from '../src/names.js';
import { EXAMPLES } from '../src/examples.js';
import { EmbeddedModule } from '../src/module.js';
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
                /Delete one, or export it to a file/);
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

// --- the single-file build --------------------------------------------------

await test('the built page contains every module, with nothing left to import', async () => {
  const code = bundle('app.js');
  for (const name of ['app.js', 'module.js', 'library.js', 'perform.js', 'controller.js',
                      'audio.js', 'patches.js', 'views.js', 'midi.js', 'protocol.js']) {
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

// --- the harness ------------------------------------------------------------

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
