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
import { fromPatchJson, toPatchJson } from '../src/patchjson.js';
import { validate } from '../src/validate.js';
import { SCALES, scaleMaskOf, STEP_DIRECTIONS, METRONOME_DIVISIONS, METRONOME_FEELS } from '../src/names.js';
import { EXAMPLES } from '../src/examples.js';
import { EmbeddedModule } from '../src/module.js';
import { slider } from '../src/views.js';
import {
  connectNewNode, patchBlocks, connectionsOf, planConnection, planDisconnect, planClear,
  applyWrite, freeBus, waitingBus,
} from '../src/graph.js';
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
                      'audio.js', 'library.js', 'scope.js', 'views.js', 'midi.js', 'protocol.js']) {
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

// --- the harness ------------------------------------------------------------

// Enough of a document for `el()` to build an element and for a test to fire
// events at it. views.js touches nothing else, and a fake this small is
// honest: the sequences fired at it come from a real browser.
function fakeDocument() {
  return {
    createElement(tag) {
      const listeners = new Map();
      return {
        tag, nodeType: 1, className: '', attrs: {}, value: '',
        setAttribute(key, value) { this.attrs[key] = String(value); if (key === 'value') this.value = String(value); },
        addEventListener(type, fn) {
          if (!listeners.has(type)) listeners.set(type, []);
          listeners.get(type).push(fn);
        },
        append() {},
        fire(type, event = {}) { for (const fn of listeners.get(type) ?? []) fn({ type, ...event }); },
      };
    },
  };
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
