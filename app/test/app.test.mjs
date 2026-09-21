// The parts of the app that are not the protocol: the patch library, the
// module's runtime seam, and the panels.
//
// `protocol.test.mjs` checks the app against the firmware's protocol. This
// checks the three things the merge added, and it checks them the same way -
// against the real module where a module is involved:
//
//   * a patch saved in the browser comes back as the patch it was - in words,
//     so it survives the next change to the image - and a full quota says so
//     rather than losing a patch quietly;
//   * an incoming CC takes main.cpp's path through the control plane, which is
//     what makes learn work from a controller plugged into the browser;
//   * a slider does not take a value from a finger that was scrolling past it;
//
//   npm test -- app      (vitest; MMMC_WASM names another module)

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { test } from 'vitest';

import * as P from '../src/protocol/generated.js';
import * as codec from '../src/protocol/codec.js';
import { Library, ago } from '../src/services/storage.js';
import { createState } from '../src/services/state.js';
import { Arrangement } from '../src/services/arrangement.js';
import { Editor } from '../src/services/editor.js';
import { Patches } from '../src/services/patches.js';
import { fromPatchJson, toPatchJson, SEQ_FAMILY } from '../src/core/patchjson.js';
import { validate, advise } from '../src/core/validate.js';
import {
  SCALES, scaleMaskOf, scaleIdOf, STEP_DIRECTIONS, METRONOME_DIVISIONS, METRONOME_FEELS, ALL_MUSICAL,
} from '../src/protocol/names.js';
import { keySpelling, triadQuality, degreeOf, scaleTriad, romanNumeral,
         fifthsFrom } from '../src/core/music.js';
import { EXAMPLES, EXAMPLE_CATEGORIES, exampleGroups } from '../src/core/examples.js';
import { isMacro, isBinding, destsOfMacro } from '../src/core/patch.js';
import { patchSchema, promptText, schemaText, WORKED_EXAMPLE } from '../src/core/schema.js';
import { paramSections, paramSection, PARAM_SECTIONS } from '../src/ui/controls/ParamSections.js';
import { Slider } from '../src/ui/components/Slider.js';
import { LearnButton } from '../src/ui/controls/LearnButton.js';
import { NodeCard } from '../src/ui/panels/NodeCard.js';
import { CanvasPanel, geometry, canvasKey } from '../src/ui/canvas/Canvas.js';
import { KeyBadge, rootInlet } from '../src/ui/components/KeyBadge.js';
import { NO_STEP } from '../src/ui/panels/grids/playhead.js';
import { describeTarget, RouteTable } from '../src/ui/panels/ModMatrix.js';
import { Macros } from '../src/ui/panels/Macros.js';
import { Keyboard } from '../src/ui/components/Keyboard.js';
import { Surface as SurfaceView } from '../src/ui/surface/Surface.js';
import { AssignSheet } from '../src/ui/surface/Assign.js';
import { Surface as SurfaceDoc, PadKind, PadMode } from '../src/services/surface.js';
import { Play } from '../src/services/play.js';
import { Transport } from '../src/services/transport.js';
import { Transport as TransportView } from '../src/ui/components/Transport.js';
import { Device } from '../src/protocol/device.js';
import { KeyTab } from '../src/ui/tabs/KeyTab.js';
import { SchemaTab } from '../src/ui/tabs/SchemaTab.js';
import { shade, nodeRollSources, scopeRows, blockSignals } from '../src/ui/scope/scope.js';
import { BlockSignalsPanel } from '../src/ui/scope/ScopePanels.js';
import { BlockKind } from '../src/core/graph.js';
import { ICON_NAMES } from '../src/ui/components/icons.js';
import { gateHits } from '../src/runtime/audio/listener.js';
import { KITS, LANE_NOTES, PIECES, drumSources, hit, pieceOf, voiceSpec } from '../src/runtime/audio/drums.js';
import {
  patchBlocks, connectionsOf, planConnection, planDisconnect, planClear,
  applyWrite, freeBus, planJackDirection, planPortFlip, applyPortFlip,
  planPortFanOut, planModulation, planBusModulation, planCcBinding, CC_MAX,
  copyNode, nodeFromCopy,
} from '../src/core/graph.js';
import { catalogue, filterGroups, optionsOf, ENDPOINTS } from '../src/core/catalogue.js';
import {
  autoLayout, layoutOf, socketPoint, blockHeight, forgetNode, underBlock, BLOCK_W, ROW_H,
} from '../src/core/layout.js';
import { MidiOutputs, cablesOut, messageBytes } from '../src/runtime/midiout.js';
import { OUTPUT_LATENCY_MS } from '../src/runtime/module.js';
import { Heartbeat } from '../src/runtime/heartbeat.js';
import { OutputPanel, ClockPanel } from '../src/ui/tabs/MidiTab.js';
import { CLOCK_MIDI_SOURCE } from '../src/protocol/names.js';
import {
  instantiate, connected, fakeApp, fakeStorage, fakeAudioContext, fakeAudioThread,
  fakePage, listening, patched, words, find, findAll, withDom, repoRoot,
} from './harness/index.mjs';

// --- the library ------------------------------------------------------------

test('a saved patch comes back as the patch it was', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const library = new Library(fakeStorage());
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(1));
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_OUT, buses: [3] };
  const globals = codec.emptyGlobals();
  globals.bpm = 137;

  const entry = library.save({ name: 'a patch', patch: toPatchJson(patch, globals, device), nodes: 1 });
  const read = library.get(entry.id);
  const back = fromPatchJson(read.patch, device);
  assert.equal(back.globals.bpm, 137);
  assert.equal(back.patch.nodes.length, 1);
  assert.deepEqual(back.patch.gatePorts[0].buses, [3]);
});

// The reason the library holds words and not bytes. The image is versioned and
// this project renumbers ids and moves fields whenever the shape is wrong, so
// a library of images empties itself on the next such change. A stored patch
// names its algorithms, so the same file still loads on the other side of one.
test('a stored patch does not depend on the image it was saved from', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const library = new Library(fakeStorage());
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(device.algorithms.find(Boolean).id));
  const stored = toPatchJson(patch, codec.emptyGlobals(), device);
  library.save({ name: 'named', patch: stored, nodes: 1 });

  const [row] = library.read();
  assert.equal(typeof row.patch.nodes[0].algo, 'string',
               'the node is stored by name, not by an id the next firmware may renumber');
  assert.equal(row.image, undefined, 'no image is kept beside it');
});

test('the library lists, renames, duplicates and deletes', async () => {
  const library = new Library(fakeStorage());
  const doc = { nodes: [] };
  const first = library.save({ name: 'one', patch: doc, nodes: 0 });
  const second = library.save({ name: 'two', patch: doc, nodes: 2 });

  // Newest first: the list exists to answer "what was I just working on?".
  assert.deepEqual(library.list().map((e) => e.name), ['two', 'one']);
  // And it carries no patches: a list of patches must not drag every patch
  // through the render path.
  assert.ok(library.list().every((e) => e.patch === undefined));

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
  library.save({ id: first.id, name: 'renamed', patch: doc, nodes: 1 });
  assert.equal(library.list().length, 2);
});

test('a full quota is reported, not swallowed', async () => {
  const library = new Library(fakeStorage({ limit: 40 }));
  assert.ok(library.available, 'the probe write fits');
  const doc = { nodes: [{ algo: 'Metronome' }] };
  assert.throws(() => library.save({ name: 'too big', patch: doc, nodes: 1 }),
                /delete a patch, or export it to a file/);
  // The working patch is autosaved on every edit, so it must never throw: a
  // full quota may not be allowed to break editing.
  library.saveWorking({ id: null, name: 'working', patch: doc });
});

test('a browser that stores nothing is a message, not a crash', async () => {
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
  library.saveWorking({ id: null, name: 'x', patch: { nodes: [] } });
});

test('the working patch survives a reload', async () => {
  const storage = fakeStorage();
  const doc = { nodes: [{ algo: 'Metronome' }], globals: { bpm: 91 } };
  new Library(storage).saveWorking({ id: 'p1', name: 'in progress', patch: doc });
  const restored = new Library(storage).readWorking();
  assert.equal(restored.name, 'in progress');
  assert.equal(restored.id, 'p1');
  assert.deepEqual(restored.patch, doc);
});

// The library, driven the way the library tab drives it. Enough of the app for
// `Patches` to run headless: a real library over a fake browser storage, and
// an editor that sends nowhere.
function patchService({ storage, device }) {
  const state = createState();
  const library = new Library(storage);
  const editor = { device, sendWhole: () => {}, say: () => {}, fail: (m) => { state.error = m; } };
  const arrangement = new Arrangement({ state, library });
  return { state, library, patches: new Patches({ state, library, editor, arrangement, render: () => {} }) };
}

// What the whole change is for: a patch saved in one session opens in the
// next. Saved, then read back by a page that shares nothing with the one that
// saved it but the browser's storage.
test('a patch saved in the library opens again in a new page', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const storage = fakeStorage();
  const [name, example] = Object.entries(EXAMPLES).find(([, e]) => e.category === 'performance');
  const { patch, globals } = fromPatchJson(example.patch, device);

  const first = patchService({ storage, device });
  first.state.patch = patch;
  first.state.globals = globals;
  first.state.current.name = name;
  first.patches.save();
  const id = first.state.current.id;
  assert.ok(id, `saving ${name} failed: ${first.state.error}`);
  assert.equal(first.state.current.dirty, false);

  const next = patchService({ storage, device });
  next.patches.load(id);
  assert.equal(next.state.error, null, `loading ${name} back failed`);
  assert.deepEqual([...codec.encodePatch(next.state.patch, next.state.globals)],
                   [...codec.encodePatch(patch, globals)], `${name} changed on the way through`);
  // And it says so: a patch nobody has touched since it was loaded must not
  // claim there is something to save.
  next.patches.autosave({ now: true });
  assert.equal(next.state.current.dirty, false, 'a patch just loaded already says it has changed');

  // The reload: the working patch comes back, still attached to the entry it
  // was loaded from.
  const reopened = patchService({ storage, device });
  reopened.patches.restoreWorking();
  assert.equal(reopened.state.current.id, id);
  assert.equal(reopened.state.current.name, name);
  assert.equal(reopened.state.current.dirty, false);
  assert.deepEqual([...codec.encodePatch(reopened.state.patch, reopened.state.globals)],
                   [...codec.encodePatch(patch, globals)]);
});

test('ago() reads as English', async () => {
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

test('the module runs, keeps time and lights the LEDs', async () => {
  const { module } = await instantiate();
  // A freshly booted module runs the default patch, whose clock is internal.
  // Start is pressed the way the app presses it: over the protocol, because
  // the transport has no other way in from the cable the app holds.
  await new Device(module).pressTransport(P.CcTransportTarget.CC_TRANSPORT_START);
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

// --- the module while nobody is looking --------------------------------------

// A hidden page gets no animation frames and has its timers clamped, so the
// heartbeat (`runtime/heartbeat.js`) is what runs the passes there. Both
// halves are checked: that the page's own clocks stand down when it is hidden,
// and that a beat off the audio thread runs what they owed.
test('the heartbeat runs the module while the page is hidden', async () => {
  const { module } = await instantiate();
  const page = fakePage();
  const audio = fakeAudioThread();
  const had = { doc: globalThis.document, frame: globalThis.requestAnimationFrame };
  globalThis.document = page;
  globalThis.requestAnimationFrame = () => 0;      // no frames here, as in a hidden page
  const heartbeat = new Heartbeat(module, { doc: page });
  try {
    heartbeat.arm();
    page.fire('pointerdown');                      // audio starts from a gesture, never on its own
    await heartbeat.ready;
    assert.equal(audio.ctx.state, 'suspended', 'a page being looked at holds no audio device open');

    module.start();
    page.hide();
    await heartbeat.ready;
    assert.equal(audio.ctx.state, 'running', 'hidden, the audio thread is the clock');

    const before = module.now;
    await new Promise((done) => setTimeout(done, 40));
    assert.equal(module.now, before, 'the interval timer stands down in a hidden page');
    audio.beats(1);
    assert.ok(module.now - before >= 30_000,
      `a beat runs the passes the wall clock owed, got ${(module.now - before) / 1000} ms`);

    page.show();
    await heartbeat.ready;
    assert.equal(audio.ctx.state, 'suspended', 'back in the open, the device is let go again');
    assert.equal(heartbeat.beating, false);
    const back = module.now;
    audio.beats(4);
    assert.equal(module.now, back, 'a suspended context renders nothing to beat with');
  } finally {
    module.stop();
    await heartbeat.close();
    audio.restore();
    globalThis.document = had.doc;
    globalThis.requestAnimationFrame = had.frame;
  }
});

// Every browser without an AudioWorklet, and every page nobody has pressed
// yet. Neither is an error: the module stops with the page and skips forward
// when it comes back, which is what it did before there was a heartbeat.
test('no audio worklet is a heartbeat that says so rather than throwing', async () => {
  const page = fakePage();
  const had = { context: globalThis.AudioContext, node: globalThis.AudioWorkletNode };
  globalThis.AudioContext = undefined;
  globalThis.AudioWorkletNode = undefined;
  const ticks = [];
  const heartbeat = new Heartbeat({ tick: () => ticks.push(1) }, { doc: page });
  try {
    heartbeat.arm();
    page.fire('keydown');
    assert.equal(await heartbeat.ready, false);
    assert.match(heartbeat.reason, /worklet/);
    page.hide();
    await heartbeat.ready;
    assert.equal(heartbeat.beating, false);
    assert.equal(ticks.length, 0);
  } finally {
    globalThis.AudioContext = had.context;
    globalThis.AudioWorkletNode = had.node;
  }
});

// --- the transport and the panic button -------------------------------------

// The transport is one service for both machines, and the page's own module is
// driven through the very same SysEx a cable carries, so this is the real path
// and not a stand-in for it.
test('the transport service presses start, stop and continue on the module', async () => {
  const { module } = await instantiate();
  const transport = new Transport({ session: { device: new Device(module) }, fail: (m) => assert.fail(m) });

  transport.stop();
  assert.equal(module.clock().running, false);
  transport.start();
  assert.equal(module.clock().running, true);
  assert.equal(module.clock().count, 0, 'start runs from the top');

  module.advance(200_000);
  const at = module.clock().count;
  transport.stop();
  transport.resume();
  assert.equal(module.clock().running, true);
  assert.equal(module.clock().count, at, 'continue resumes on the count the stop kept');
  // The acks are answered on a microtask, as a real port would answer them.
  await Promise.resolve();
});

// A panic has three places to reach and the button is worthless if it misses
// one: the module's own note-offs, the notes already gone out of this
// computer's ports, and the page's own audio.
test('a panic sweeps every channel of the module and releases both sides of the page', async () => {
  const { module } = await instantiate();
  const swept = [];
  module.onMidi(({ type, d1, channel, target }) => {
    if (type === 0xb0 && d1 === 123) swept.push({ channel, target });
  });
  const released = [];
  const transport = new Transport({
    session: { device: new Device(module) },
    listener: () => ({ panic: () => released.push('listener') }),
    outputs: () => ({ panic: () => released.push('outputs') }),
    fail: (m) => assert.fail(m),
  });

  transport.panic();
  assert.deepEqual(released, ['outputs', 'listener']);
  assert.deepEqual(swept.map((m) => m.channel), Array.from({ length: 16 }, (_, i) => i + 1));
  // Every cable that carries music, which is all of them but the control one.
  for (const { target } of swept) {
    assert.equal(target & P.MIDI_CONTROL_PORT, 0, 'the protocol\'s own cable is never played on');
    assert.equal(target | P.MIDI_CONTROL_PORT, 0xff, 'and every other cable is');
  }
  await Promise.resolve();
});

// The same four buttons in the shell's bar and on the surface's case. The lamp
// is the one thing the view decides for itself, and only for the module in the
// page: nothing in the protocol reports a cabled module's clock, and a lamp
// that guessed would be worse than none.
test('the transport draws four buttons and lights while the clock runs', () => {
  const pressed = [];
  const app = {
    ...fakeApp({ module: { clock: () => ({ running: true }) } }),
    transport: {
      start: () => pressed.push('start'), stop: () => pressed.push('stop'),
      resume: () => pressed.push('resume'), panic: () => pressed.push('panic'),
    },
  };
  withDom(() => {
    const group = TransportView(app);
    const buttons = findAll(group, (n) => n.tag === 'button');
    assert.equal(buttons.length, 4);
    for (const button of buttons) button.fire('click');
    assert.deepEqual(pressed, ['start', 'stop', 'resume', 'panic']);
    assert.match(words(group), /panic/, 'the one button whose word is worth its width');
    assert.ok(!group.className.includes('running'), 'nothing is lit until a frame says so');
    app.live.tick({ module: app.module });
    assert.ok(group.className.includes('running'));

    // On the instrument panel it is glyphs only: the case has no room for prose.
    assert.ok(!words(TransportView(app, { compact: true })).includes('panic'));
  });
});

// Simulated time is the wall clock. The bug this closes: the module was run
// one animation frame of simulated time per animation frame of real time,
// floored to whole passes - and a frame is 16.667 ms, so two thirds of a
// millisecond went missing every frame. Four percent slow at 60 Hz, thirteen
// at 144 Hz, and a different figure whenever a frame came late: a clock that
// was neither right nor steady, and drifted further the busier the page was.
test('simulated time follows the wall clock, odd fractions included', async () => {
  const { module } = await instantiate();
  module.syncTo(0);
  for (let frame = 1; frame <= 60; frame++) module.advanceTo(frame * 1000 / 60);
  assert.ok(Math.abs(module.now - 1_000_000) < 1000, `a second of frames ran ${module.now} us`);
  // And the same second of frames at a display that does not divide a
  // millisecond nicely at all.
  const before = module.now;
  for (let frame = 1; frame <= 144; frame++) module.advanceTo(1000 + frame * 1000 / 144);
  assert.ok(Math.abs(module.now - before - 1_000_000) < 1000, `at 144 Hz a second ran ${module.now - before} us`);
  assert.equal(module.wallAt(module.now), module.now / 1000, 'wall time and simulated time are one clock');
});

test('a late frame is caught up in full, a lost tab is skipped', async () => {
  const { module } = await instantiate();
  module.syncTo(0);
  module.advanceTo(100);
  assert.equal(module.now, 100_000);
  // A rebuild of the page held the thread for two hundred milliseconds: the
  // passes it owed run now, and the module is on time again.
  module.advanceTo(300);
  assert.equal(module.now, 300_000, 'the late frame lost time');
  // A tab that was in the background for five seconds does not play five
  // seconds of music in one go: the module skips to the present, keeping
  // only the last stretch it can catch up.
  module.advanceTo(5300);
  assert.ok(module.now < 1_000_000, `the module ran ${module.now} us for a five-second gap`);
  assert.equal(module.wallAt(module.now), 5300, 'and simulated now is wall now again');
});

test('running ahead of a rebuild costs nothing afterwards', async () => {
  const { module } = await instantiate();
  module.syncTo(0);
  module.advanceTo(10);
  // The page is about to rebuild itself: the module runs sixty milliseconds
  // ahead, and everything those passes played is stamped at its own time.
  module.advanceTo(70);
  assert.equal(module.now, 70_000);
  assert.equal(module.wallAt(70_000), 70, 'a note played in the run-ahead is timed by the wall, not by when it ran');
  // The rebuild took thirty milliseconds; the wall clock is at forty, the
  // module is at seventy, and it waits.
  module.advanceTo(40);
  assert.equal(module.now, 70_000, 'the module went backwards, or ran passes it was not owed');
  module.advanceTo(75);
  assert.equal(module.now, 75_000);
});

test('a jack tap is an edge, not a level', async () => {
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
test('a pulse too short to paint still reaches the lights', async () => {
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
test('the scope keeps the pulse the eye missed', async () => {
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
test('the MIDI log still reports events once the ring is full', async () => {
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

test('the piano roll opens a note, closes it, and holds an open one', async () => {
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
test('a note bus can be listened to, event by event', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const heard = [];
  module.onNoteBus((event) => heard.push(event));

  // MIDI in on USB 1 onto note bus 0, and *nothing* patched to a MIDI output:
  // the module sends not one byte, and the bus carries everything.
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
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

test('the monitor setup survives a reload', async () => {
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

test('a bound CC moves a parameter and is consumed', async () => {
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

// --- binding a CC without turning it ----------------------------------------
//
// Learn was the whole of the CC side: arm the module, turn a knob. A patch
// written for a controller in the next room could be given a control signal
// from the CV button beside any parameter and a CC only by carrying the patch
// to the controller - and a CC number is printed on the front of the thing.
// So a number can be named, and it makes the binding a learn would have made.
test('a CC binds by its number, with no controller in the room', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const descriptor = device.algorithms.find((a) => a.nParams > 0 && a.minIn === 0);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(descriptor.id));
  const caps = device.capabilities;

  const made = planCcBinding(patch, caps, 0, 0, 74, { device });
  assert.ok(made.ok, made.why);
  const [{ slot, mapping }] = made.bindings;
  assert.equal(slot, 0, 'the first free slot');
  assert.equal(mapping.cc, 74);
  assert.equal(mapping.channel, 0, 'a binding nobody has played is omni');
  assert.equal(mapping.sourceMask, ALL_MUSICAL, 'it cannot know which cable the knob arrives on');
  assert.equal(mapping.sourceMask & P.MIDI_CONTROL_PORT, 0, 'never the control cable');
  assert.equal(mapping.targetIndex, 0);
  assert.equal(mapping.param, 0);

  // Changing the number is an edit of the binding, not a second one: whatever
  // has been narrowed about it stays narrowed.
  patch.ccMap[slot] = { ...mapping, channel: 3, min: 10, max: 40, flags: 1 };
  const again = planCcBinding(patch, caps, 0, 0, 30, { device });
  assert.ok(again.ok, again.why);
  assert.equal(again.bindings[0].slot, slot, 'a bound parameter keeps its slot');
  assert.deepEqual(again.bindings[0].mapping,
                   { ...patch.ccMap[slot], cc: 30 },
                   'changing the number forgot the rest of the binding');

  // What is not a control is refused here rather than by the module: 120 and
  // above are channel mode messages.
  assert.equal(planCcBinding(patch, caps, 0, 0, CC_MAX + 1, { device }).ok, false);
  assert.equal(planCcBinding(patch, caps, 9, 0, 1, { device }).ok, false, 'no such node');

  // A full table says so, and says what the module holds.
  const full = codec.emptyPatch();
  full.nodes.push(codec.emptyNode(descriptor.id));
  // Bound to another parameter, every one of them: a table with a row for
  // *this* one is an edit, not a table that is full.
  for (let i = 0; i < caps.ccMappings; i++) full.ccMap[i] = { ...mapping, cc: i, param: 1 };
  const refused = planCcBinding(full, caps, 0, 0, 1, { device });
  assert.equal(refused.ok, false);
  assert.match(refused.why, /in use/);
});

// And the binding it plans is one the module honours - on any musical cable,
// which is the point of the mask it starts with.
test('a binding made by name is one the module plays', async () => {
  const { module, E } = await instantiate();
  const device = await connected(module);
  const descriptor = device.algorithms.find((a) => a.nParams > 0 && a.minIn === 0);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(descriptor.id));
  await device.sendPatch(patch, codec.emptyGlobals());

  const plan = planCcBinding(patch, device.capabilities, 0, 0, 74, { device });
  const { slot, mapping } = plan.bindings[0];
  await device.setCcMap(slot, mapping);

  // Not the cable a learn would have caught it on - there was no learn.
  const port = P.MidiPort.mmMIDI_DIN_0 ?? P.MidiPort.mmMIDI_USB_1;
  assert.equal(module.deliverMidi(port, 0xb0, 7, 74, 100), null,
               'a bound CC never reaches the graph');
  module.advance(5000);
  assert.ok(await device.getParam(0, 0) > 0, 'the parameter did not move');
  assert.equal(E.emu_last_error(), 0);
});

// The button itself: pressing it opens the menu, and what is in the menu is
// what can be done from where the user is standing.
test('the learn button opens a menu that arms and binds', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const descriptor = device.algorithms.find((a) => a.nParams > 0 && a.minIn === 0);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(descriptor.id));

  const app = fakeApp({ patch, device });
  const { calls } = app;

  withDom(() => {
    // Nothing listening to a controller: the learn is a row to press, not an
    // arm, and the numbers are there to be picked.
    const button = LearnButton(app, 0, 0, 'depth', null);
    button.fire('click');
    assert.deepEqual(calls, [], 'a learn was armed with nothing to turn it');
    const menu = document.getElementById('menu');
    assert.ok(menu, 'the button opened nothing');
    const said = words(menu);
    assert.match(said, /bind a controller to depth/);
    assert.match(said, /turn a knob/, 'learn is still one press away');
    assert.match(said, /no controller yet/, 'the row does not say why it is a press and not a wait');

    const pick = find(menu, (kid) => kid.tag === 'select');
    assert.ok(pick, 'the menu has no list of CC numbers');
    assert.equal(pick.children.length, CC_MAX + 2, 'every control, and "not bound"');
    pick.fire('change', { target: { value: '74' } });
    assert.deepEqual(calls.at(-1), ['bindParam', 0, 0, 74]);
    assert.equal(document.getElementById('menu'), null, 'the menu stayed open over the page');

    // A controller is listening: opening the menu is the arming, because
    // turning a knob is what the button is called.
    calls.length = 0;
    app.controller = { input: { name: 'a keyboard' } };
    LearnButton(app, 0, 0, 'depth', null).fire('click');
    assert.deepEqual(calls, [['learn', 0, 0]], 'the press did not arm the learn');
    assert.match(words(document.getElementById('menu')), /waiting for a controller/);

    // And the button says so where the parameter is, not only in the menu.
    assert.match(LearnButton(app, 0, 0, 'depth', null).className, /armed/);
    assert.ok(!LearnButton(app, 0, 1, 'rate', null).className.includes('armed'),
              'one learn is armed at a time, and it is not every button');
  });
});

test('what the module plays reaches whoever is listening', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const heard = [];
  module.onMidi((event) => heard.push(event));

  // MIDI in on USB 1 onto a note bus, and that bus out to DIN 1: the smallest
  // patch that makes the module send something.
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 0, buses: [0] };
  await device.sendPatch(patch, codec.emptyGlobals());

  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x90, 1, 60, 100);
  module.advance(5000);
  assert.ok(heard.some((e) => e.type === 0x90 && e.d1 === 60),
            `no note on came out (${heard.length} events)`);
  assert.ok(module.midiLog.length > 0, 'the log the play tab shows is empty');
  assert.equal(heard[0].target, P.MidiPort.mmMIDI_SERIAL_1, 'the target port is carried through');
});

// --- out of this computer ----------------------------------------------------
//
// The other half of routing a controller into the page: what the module plays
// has to leave it, or the app can be played with but never *play* anything.
// The module names its own cables and the browser names ports; these check
// that the one reaches the other, and that a note already out of the page is
// taken back rather than left sounding on a synth nobody can reach.

// Ports that record what was sent, and an access holding them.
function fakeMidiOut(...names) {
  const sent = new Map(names.map((name) => [name, []]));
  const stamps = new Map(names.map((name) => [name, []]));
  const ports = names.map((name) => ({
    id: `id:${name}`, name,
    send: (bytes, at) => { sent.get(name).push([...bytes]); stamps.get(name).push(at); },
  }));
  return { access: { outputs: new Map(ports.map((port) => [port.id, port])) }, sent, stamps };
}

// The routing, over the real module or - where the module is not what is
// being checked - one that only has to hand events over and say where
// simulated time falls on the wall clock.
function routed(names, { library = null, module = null } = {}) {
  let emit = () => {};
  const { access, sent, stamps } = fakeMidiOut(...names);
  const standIn = { now: 0, wallAt: (simUs) => simUs / 1000, onMidi: (fn) => { emit = fn; } };
  const outputs = new MidiOutputs(module ?? standIn, library);
  outputs.access = access;
  return { outputs, sent, stamps, standIn, play: (event) => emit(event) };
}

test('what the module plays leaves by the port its cable is routed to', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const { outputs, sent } = routed(['a synth', 'a drum machine'], { module });
  outputs.route(P.MidiPort.mmMIDI_SERIAL_1, 'a synth');
  outputs.route(P.MidiPort.mmMIDI_USB_1, 'a drum machine');
  // The module is the one in the page, playing a real patch: in on USB 1, out
  // on DIN 1, which is the cable the synth is on.
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 0, buses: [0] };
  await device.sendPatch(patch, codec.emptyGlobals());

  module.deliverMidi(P.MidiPort.mmMIDI_USB_0, 0x90, 1, 60, 100);
  module.advance(5000);
  assert.deepEqual(sent.get('a synth'), [[0x90, 60, 100]], 'the note did not reach the synth on DIN 1');
  assert.deepEqual(sent.get('a drum machine'), [], 'a cable the patch never plays to sent something');
});

test('two cables on one synth is one note, not two', () => {
  const { outputs, sent, play } = routed(['a synth']);
  outputs.route(P.MidiPort.mmMIDI_USB_0, 'a synth');
  outputs.route(P.MidiPort.mmMIDI_SERIAL_1, 'a synth');
  // What the firmware sends: one call carrying the whole target mask.
  play({ target: P.MidiPort.mmMIDI_USB_0 | P.MidiPort.mmMIDI_SERIAL_1, type: 0x90, d1: 64, d2: 90, channel: 1 });
  assert.deepEqual(sent.get('a synth'), [[0x90, 64, 90]], 'the same port was sent to twice: a flam nobody programmed');
});

test('a message leaves as many bytes as its status says', () => {
  assert.deepEqual(messageBytes(0x90, 1, 60, 100), [0x90, 60, 100]);
  assert.deepEqual(messageBytes(0x90, 16, 60, 100), [0x9f, 60, 100], 'the firmware counts channels from 1');
  // Web MIDI refuses a message of the wrong length rather than truncating it,
  // so a Program Change sent as three bytes is a throw and a patch that
  // recalls on another box goes silent.
  assert.deepEqual(messageBytes(0xc0, 1, 7, 0), [0xc0, 7], 'a Program Change is two bytes');
  assert.deepEqual(messageBytes(0xd0, 2, 40, 0), [0xd1, 40], 'channel pressure is two bytes');
  assert.deepEqual(messageBytes(0xf8, 0, 0, 0), [0xf8], 'a clock byte has no channel and no data');
});

// A message leaves for when it happened. The passes of a frame run in a
// burst at the frame, and a rebuild of the page runs them early or late; the
// synth on the cable must hear neither. Web MIDI takes a time with the bytes,
// and the time is the event's own, on this computer's clock, plus the headroom
// the audio listener keeps - so the cable and the page's own audio agree.
test('what leaves a port is stamped with its own time, not the page\'s', () => {
  const { outputs, stamps, standIn, play } = routed(['a synth']);
  outputs.route(P.MidiPort.mmMIDI_USB_0, 'a synth');
  play({ t: 2_000_000, target: P.MidiPort.mmMIDI_USB_0, type: 0x90, d1: 60, d2: 100, channel: 1 });
  play({ t: 2_250_000, target: P.MidiPort.mmMIDI_USB_0, type: 0x80, d1: 60, d2: 0, channel: 1 });
  const [on, off] = stamps.get('a synth');
  assert.equal(on, 2000 + OUTPUT_LATENCY_MS, 'the note-on is not stamped for where its time falls on the wall');
  assert.equal(off - on, 250, 'the note is not as long on the cable as it was in the module');
  // A release for a re-route or a panic goes out for the module's own time -
  // never before a note-on already stamped later than the moment of asking.
  standIn.now = 2_100_000;
  play({ t: 2_100_000, target: P.MidiPort.mmMIDI_USB_0, type: 0x90, d1: 64, d2: 100, channel: 1 });
  outputs.panic();
  const release = stamps.get('a synth').at(-1);
  assert.ok(release >= 2100 + OUTPUT_LATENCY_MS, `the note-off at ${release} overtakes the note-on it ends`);
});

test('a note held when its cable is re-pointed is taken back', () => {
  const { outputs, sent, play } = routed(['a synth', 'another synth']);
  outputs.route(P.MidiPort.mmMIDI_USB_0, 'a synth');
  play({ target: P.MidiPort.mmMIDI_USB_0, type: 0x90, d1: 60, d2: 100, channel: 2 });

  // Only a note off ends a note, and the module's own would arrive at the new
  // port: without this the synth holds that note until it is power-cycled.
  outputs.route(P.MidiPort.mmMIDI_USB_0, 'another synth');
  assert.deepEqual(sent.get('a synth'), [[0x91, 60, 100], [0x81, 60, 0]], 'the note was left sounding');
  assert.deepEqual(sent.get('another synth'), [], 'a note nobody played was sent to the new port');

  // And what is no longer sounding is not released twice.
  play({ target: P.MidiPort.mmMIDI_USB_0, type: 0x90, d1: 64, d2: 80, channel: 2 });
  play({ target: P.MidiPort.mmMIDI_USB_0, type: 0x80, d1: 64, d2: 0, channel: 2 });
  outputs.panic();
  assert.deepEqual(sent.get('another synth'), [[0x91, 64, 80], [0x81, 64, 0]], 'a panic sent a note off nobody was holding');
});

test('a route is remembered by the name on the desk', () => {
  const storage = fakeStorage();
  const first = routed(['a synth'], { library: new Library(storage) });
  first.outputs.route(P.MidiPort.mmMIDI_SERIAL_2, 'a synth');

  // A reload: the id Web MIDI made up is gone, the name is not.
  const { outputs } = routed(['a synth'], { library: new Library(storage) });
  assert.equal(outputs.routeOf(P.MidiPort.mmMIDI_SERIAL_2), 'a synth', 'the routing was forgotten on reload');
  assert.ok(outputs.output(P.MidiPort.mmMIDI_SERIAL_2), 'the remembered name found no port');
});

// Where the clock is routed to and from: two cable masks, sent as the one
// message that carries them. Realtime reaches no bus, so neither of them is a
// MIDI routing row and both belong to the clock.
test('the clock panel routes the clock in and out', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const globals = { ...codec.emptyGlobals() };
  const app = fakeApp({ device, module, globals });

  withDom(() => {
    const panel = ClockPanel(app);
    const said = words(panel);
    assert.match(said, /follows these ports/);
    assert.match(said, /sends clock to/);

    const group = find(panel, (n) => n.attrs?.['aria-label'] === 'clock output ports');
    find(group, (n) => words(n).trim() === 'DIN 1').fire('click');
  });
  assert.equal(app.state.globals.clockOutMask, P.MidiPort.mmMIDI_SERIAL_1);
  assert.deepEqual(app.calls.at(-1), ['clockRoute', { clockOutMask: P.MidiPort.mmMIDI_SERIAL_1 }]);

  // Following a cable and clocking it back is the module clocking itself. On
  // two DIN sockets it is an ordinary chain, so it is said rather than
  // refused - and only said when MIDI is what the clock is following.
  assert.doesNotMatch(withDom(() => words(ClockPanel(app))), /clocking itself/);
  app.state.globals.clockInMask = P.MidiPort.mmMIDI_SERIAL_1;
  app.state.globals.clockSource = CLOCK_MIDI_SOURCE;
  const warned = withDom(() => words(ClockPanel(app)));
  assert.match(warned, /DIN 1 both follows and sends/);
});

// A cable carrying nothing but clock is still a cable something is on:
// clocking a drum machine and playing it no notes is an ordinary patch.
test('a cable the clock alone goes out of is offered a route', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const globals = { ...codec.emptyGlobals(), clockOutMask: P.MidiPort.mmMIDI_SERIAL_2 };
  assert.deepEqual(cablesOut(patch, device.capabilities, globals).map((c) => c.label), ['DIN 2']);
});

test('the output panel asks once per cable the patch plays to', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  patch.midiOut[1] = { targetMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 0, buses: [1] };
  assert.deepEqual(cablesOut(patch, device.capabilities).map((c) => c.label), ['USB 1', 'DIN 1']);

  const { outputs, sent } = routed(['a synth']);
  const app = fakeApp({ patch, device, module });
  app.outputs = outputs;

  withDom(() => {
    const panel = OutputPanel(app);
    const said = words(panel);
    assert.match(said, /USB 1/);
    assert.match(said, /DIN 1/, 'a cable the patch plays to was not offered a port');
    const picks = findAll(panel, (kid) => kid.tag === 'select');
    assert.equal(picks.length, 2, 'one select per cable in use');
    // Every port on the computer, and "nothing", which is where a cable starts.
    assert.deepEqual(picks[0].children.map((option) => option.attrs.value), ['', 'a synth']);

    picks[1].fire('change', { target: { value: 'a synth' } });
    assert.equal(outputs.routeOf(P.MidiPort.mmMIDI_SERIAL_1), 'a synth', 'picking a port routed nothing');
    outputs.forward({ target: P.MidiPort.mmMIDI_SERIAL_1, type: 0x90, d1: 48, d2: 70, channel: 1 });
    assert.deepEqual(sent.get('a synth'), [[0x90, 48, 70]]);

    // A patch that plays nothing out is told so rather than shown an empty box.
    const quiet = fakeApp({ patch: codec.emptyPatch(), device, module });
    quiet.outputs = outputs;
    assert.match(words(OutputPanel(quiet)), /plays nothing out/);
  });
});

// --- the example patches ----------------------------------------------------

// Every example, into the real firmware. An example that no longer loads is a
// broken front door: it is the first thing a visitor presses.
test('every example patch is one the firmware accepts', async () => {
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

// The picker shelves them, so an example on a shelf nobody drew is an example
// nobody finds. `exampleGroups` puts an unknown category on a shelf of its own
// rather than dropping it, which is right at runtime and wrong in a commit.
test('every example is on a shelf the picker knows', () => {
  const groups = exampleGroups();
  const listed = groups.flatMap((g) => g.options.map((o) => o.value));
  assert.deepEqual([...listed].sort(), Object.keys(EXAMPLES).sort(), 'the picker lost or doubled an example');
  for (const [name, example] of Object.entries(EXAMPLES)) {
    assert.ok(EXAMPLE_CATEGORIES.includes(example.category),
              `"${name}" is in category "${example.category}", which is not one of the shelves`);
  }
  for (const group of groups) {
    for (const option of group.options) {
      assert.ok(option.note, `${option.value} says nothing beside its name`);
      assert.ok(option.hint && option.hint.length < EXAMPLES[option.value].about.length + 1,
                `${option.value} has no summary line`);
    }
  }
});

// The performance presets are the reason the JSON dialect learned about
// macros. A preset whose pots quietly stopped travelling in the file would
// still load, still play, and no longer be the thing it says it is.
test('the performance presets carry macros a pot can reach', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const played = Object.entries(EXAMPLES).filter(([, e]) => e.category === 'performance');
  assert.ok(played.length >= 4, `only ${played.length} presets to play`);
  for (const [name, example] of played) {
    const { patch, globals } = fromPatchJson(example.patch, device);
    const macros = patch.macros.filter(isMacro);
    assert.ok(macros.length >= 4, `${name} has ${macros.length} macros`);
    for (const [index, macro] of patch.macros.entries()) {
      if (!isMacro(macro)) continue;
      assert.ok(destsOfMacro(patch, index).length, `${name}: "${macro.name}" moves nothing`);
      // And something has to move the macro, or it is a name and no gesture.
      assert.ok(patch.ccMap.some((m) => isBinding(m)
        && m.targetKind === P.CcTargetKind.CC_TARGET_MACRO && m.targetIndex === index),
        `${name}: nothing is bound to "${macro.name}"`);
    }
    // The module takes them, and gives them back: a macro's name and every
    // window of its travel survive the patch image.
    await device.sendPatch(patch, globals);
    for (const [index, macro] of patch.macros.entries()) {
      if (!isMacro(macro)) continue;
      assert.equal((await device.getMacro(index))?.name, macro.name, `${name}: macro ${index} came back wrong`);
    }
  }
});

// The masks are generated from midi/scale.h; this is the check that the names
// beside them still point at what the firmware would play.
test('the scale names name the firmware\'s scales', async () => {
  const { E } = await instantiate();
  // Id 0 is not a scale but an unset byte, and the list leaves it out.
  assert.equal(SCALES.length, E.emu_scale_count() - 1, 'a scale has been added or removed');
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
test('the note values and directions name the firmware\'s own options', async () => {
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
// The key is one setting for the whole patch and no node carries a copy, so a
// knob reaches it by kind rather than by node and parameter - and the app has
// to offer that, or the key is the one thing on the module a controller
// cannot move.
test('a knob and a modulator can be pointed at the key', async () => {
  const { module } = await instantiate();
  const device = await connected(module);

  const patch = codec.emptyPatch();
  patch.ccMap[0] = {
    sourceMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 1, cc: 20,
    targetKind: P.CcTargetKind.CC_TARGET_KEY, targetIndex: 0,
    param: P.CcKeyTarget.CC_KEY_ROOT, min: 0, max: 0, flags: 0,
  };
  patch.modMap[0] = {
    bus: 0, targetKind: P.CcTargetKind.CC_TARGET_KEY, targetIndex: 0,
    param: P.CcKeyTarget.CC_KEY_SCALE, min: 0, max: 0, depth: 255, flags: 0,
  };
  const app = fakeApp({ patch, device });
  assert.equal(describeTarget(app, patch.ccMap[0]), 'key · root');
  assert.equal(describeTarget(app, patch.modMap[0]), 'key · scale');

  // The module is the judge of whether either is a target at all.
  await device.sendPatch(patch, codec.emptyGlobals());
});

// The key tab is the only place a scale is chosen, so it has to show what the
// choice does: a keyboard with the notes of the key lit, the root ringed, and
// every key a way of moving the root.
test('the key tab draws the scale on a keyboard, and a key moves the root', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const globals = { ...codec.emptyGlobals(), scale: P.ScaleId.SCALE_NATURAL_MINOR, root: 9 };
  const app = fakeApp({ device, globals });

  const panel = withDom(() => KeyTab(app));
  const board = find(panel, (n) => n.className === 'keyboard');
  assert.ok(board, 'there is a keyboard');
  // Two octaves: fourteen white keys and ten black ones, in piano order.
  const keys = board.children;
  assert.equal(keys.filter((k) => k.className.includes('kb-white')).length, 14);
  assert.equal(keys.filter((k) => k.className.includes('kb-black')).length, 10);

  // A minor is A B C D E F G: seven of the twelve pitch classes, lit twice
  // over, and A is the one ringed.
  const lit = keys.filter((k) => k.className.includes(' in'));
  assert.equal(lit.length, 14, 'seven notes, two octaves');
  const ringed = keys.filter((k) => k.className.includes('root'));
  assert.equal(ringed.length, 2);
  assert.ok(ringed.every((k) => k.attrs['aria-label'] === 'root A'));
  // Every lit key says which degree of the key it is; a key outside it says so.
  assert.ok(lit.every((k) => /degree [1-7] of the key/.test(k.attrs.title)));
  const dark = keys.find((k) => k.attrs['aria-label'] === 'root C#');
  assert.match(dark.attrs.title, /not in the key/);
  assert.match(words(panel), /A minor — A B C D E F G/);

  // And in a flat key it says so: the panel claims to be what a musician
  // writes down, so it cannot write the third of C minor as D sharp.
  globals.root = 0;
  const flat = withDom(() => KeyTab(app));
  assert.match(words(flat), /C minor — C D E\u266d F G A\u266d B\u266d/);
  assert.ok(find(flat, (n) => n.attrs?.['aria-label'] === 'root E\u266d'),
            'the keyboard is spelled the same way as the sentence under it');

  // And a key is a control, not a picture.
  dark.fire('click');
  assert.equal(globals.root, 1, 'pressing C# put the module in C#');
  assert.deepEqual(app.calls.at(-1), ['globals', { root: 1 }, 'key'], 'and it was sent as the key');
});

// A pitch class is a number and a number has no spelling, so a list of sharps
// is right about half the time. The half it is wrong about is the half where
// it stops being a spelling question: a chord called D# where E flat belongs,
// on the third degree of C minor, reads as the degree being wrong, because D
// is the second.
//
// The rule is the one a musician uses: a seven-note scale uses each letter
// once, in order, so the letter of degree i is the tonic's plus i and the
// accidental is whatever gets that letter to the pitch.
test('a key spells its own notes', async () => {
  const notes = (root, scale) => {
    const mask = scaleMaskOf(scale);
    const spelling = keySpelling(root, mask);
    const out = [];
    for (let step = 0; step < 12; step++) if ((mask >> step) & 1) out.push(spelling[(root + step) % 12]);
    return out.join(' ');
  };

  assert.equal(notes(0, 'minor'), 'C D E\u266d F G A\u266d B\u266d', 'C minor is flat, and this is the bug');
  assert.equal(notes(0, 'major'), 'C D E F G A B');
  assert.equal(notes(9, 'minor'), 'A B C D E F G');
  assert.equal(notes(4, 'major'), 'E F# G# A B C# D#', 'a sharp key stays sharp');
  assert.equal(notes(10, 'major'), 'B\u266d C D E\u266d F G A');
  // Each letter once, even where that costs an accidental on a white note:
  // C sharp minor has an E and not an F, because the third is an E-something.
  assert.equal(notes(1, 'minor'), 'C# D# E F# G# A B');
  // Six sharps against six flats is a tie, and only one of them is written.
  assert.equal(notes(3, 'minor'), 'E\u266d F G\u266d A\u266d B\u266d C\u266d D\u266d');
  // A double flat is legal and a triple is not, which is what rules out
  // spelling C sharp minor from D.
  assert.equal(notes(0, 'harmonic minor'), 'C D E\u266d F G A\u266d B');

  // A scale that is not seven notes has no letter-per-degree to follow, so it
  // falls back to one decision for the whole key rather than inventing one.
  assert.equal(notes(5, 'pentatonic major'), 'F G A C D');
  assert.equal(notes(0, 'chromatic').split(' ').length, 12);

  // Every key names all twelve pitch classes, because the circle draws the
  // five the key has not got as well.
  for (let root = 0; root < 12; root++) {
    for (const scale of ['major', 'minor', 'dorian', 'blues', 'whole tone']) {
      const spelling = keySpelling(root, scaleMaskOf(scale));
      assert.equal(spelling.length, 12);
      assert.ok(spelling.every((name) => typeof name === 'string' && name.length),
                `${root} ${scale}: a pitch class with no name`);
    }
  }
});

// that appears nowhere is a route nobody can find or remove.
test('a route with no block to land on is still listed', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.modMap[0] = {
    bus: 0, targetKind: P.CcTargetKind.CC_TARGET_CLOCK, targetIndex: 0,
    param: P.CcClockTarget.CC_CLOCK_TEMPO, min: 0, max: 0, depth: 255, flags: 0,
  };
  const app = fakeApp({ patch, device });
  assert.match(describeTarget(app, patch.modMap[0]), /clock/);
  assert.ok(withDom(() => RouteTable(app)),
            'a clock route has nowhere to be drawn, so the table is where it lives');
});

// --- the details panel ---------------------------------------------------------
//
// A node's parameters are sorted onto the same few sections on every card -
// which notes, when, how loud, how likely - rather than left in the order the
// firmware stores them. The sorting is arithmetic on the descriptors, so it
// is checked here against every algorithm the module reports.
test('parameters are filed by what they do, on every node', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const named = (name) => device.algorithms.find((d) => d?.name === name);
  const where = (d, param) => paramSections(d.params)
    .find((section) => section.params.some((p) => p.pd.name === param))?.key;

  // The same word lands in the same place whichever node it is on - on every
  // node that leaves the editor to guess, which is nearly all of them.
  for (const name of ['Chord', 'CvToNote']) {
    const d = named(name);
    assert.ok(d, `${name} is in this firmware`);
    assert.equal(where(d, 'octave'), 'pitch', `${name}: octave`);
    assert.equal(where(d, 'velocity'), 'level', `${name}: velocity`);
  }

  // And an algorithm that labels its groups is believed instead of guessed
  // at. Harmony's five controls over one chord walk were spread over three
  // headings by the words alone - `spread` is a pitch word from NoteDelay,
  // `smooth` a level word from a filter - and they are one section because
  // the firmware says which section (src/node/param.h).
  const harmony = named('Harmony');
  assert.ok(harmony, 'Harmony is in this firmware');
  const walk = paramSections(harmony.params).find((section) => section.label === 'the walk');
  assert.ok(walk, 'Harmony names a section for its walk');
  assert.deepEqual(walk.params.map((p) => p.pd.name),
                   ['fifths', 'smooth', 'leading', 'spread', 'gravity', 'seed'],
                   'every control over the walk, in one place and in one order');
  assert.deepEqual(paramSections(harmony.params).map((section) => section.label),
                   ['the walk', 'the form', 'the output'],
                   'the sections are the firmware\u2019s, in the firmware\u2019s order');
  // Runs of one mechanism that the preset order splits are one section again.
  assert.ok(harmony.params.filter((g) => g?.label === 'the walk').length > 1,
            'the walk is more than one run of the parameter order');
  // Tonnetz names its own for the same reason: `cycle` and `diatonic` are
  // behaviour words and `deviation` and `seed` are chance words, so its four
  // controls over one walk were shown under two headings.
  assert.deepEqual(paramSections(named('Tonnetz').params).map((section) => section.label),
                   ['the walk', 'the output']);
  assert.equal(where(named('GateToNote'), 'channel'), 'midi');
  assert.equal(where(named('Probability'), 'seed'), 'chance');
  assert.equal(where(named('Arpeggiator'), 'mode'), 'mode');
  // The LFO names its own for a third reason: `sync` decides which of its two
  // rate settings is live, and sorting by word puts it under behaviour while
  // both of the settings it governs land under timing - so the control and
  // the two it switches between were in different sections of the card.
  assert.deepEqual(paramSections(named('LFO').params).map((section) => section.label),
                   ['shape', 'rate', 'level']);
  assert.deepEqual(paramSections(named('LFO').params)
                     .find((section) => section.label === 'rate').params.map((p) => p.pd.name),
                   ['sync', 'rate', 'division', 'feel']);
  assert.equal(where(named('LFO'), 'polarity'), 'shape',
               'two runs of the parameter order under one label are one section');
  assert.equal(paramSection({ name: 'anything at all', kind: P.ParamKind.PARAM_NUMBER }), 'other',
               'a word the table has never met is still a control');

  // Nothing is lost in the sorting: every header parameter of every
  // algorithm is on exactly one section, in the order the sections are
  // declared, and a table's fields stay out of it.
  const order = PARAM_SECTIONS.map((s) => s.key);
  for (const d of device.algorithms) {
    if (!d) continue;
    const expected = [];
    for (const group of d.params) {
      if (!group || group.repeat > 1) continue;
      for (let f = 0; f < group.nFields; f++) {
        const pd = group.fields[f];
        if (pd && !(pd.min === 0 && pd.max === 0)) expected.push(group.first + f);
      }
    }
    const sections = paramSections(d.params);
    const seen = sections.flatMap((s) => s.params.map((p) => p.at)).sort((a, b) => a - b);
    // Both sorted: this is the "nothing is lost" claim, and a descriptor may
    // declare its groups in the order it wants them shown rather than in
    // parameter order.
    assert.deepEqual(seen, expected.sort((a, b) => a - b), `${d.name}: every parameter once`);
    // A descriptor that says nothing is sorted into the standard sections, in
    // the standard order. One that labels its groups sets its own order, and
    // it is the order the labels first appear in.
    if (sections.every((s) => !s.label || order.includes(s.key))) {
      const keys = sections.map((s) => order.indexOf(s.key));
      assert.deepEqual(keys, [...keys].sort((a, b) => a - b), `${d.name}: sections in order`);
    } else {
      const labels = [];
      for (const group of d.params) {
        if (group?.label && !labels.includes(group.label)) labels.push(group.label);
      }
      // The labelled groups first, in the order the firmware declares them.
      // A descriptor may label only some of its groups - Retrigger labels
      // what a strike is and leaves its output channel out of it - and what
      // it leaves unlabelled follows, sorted into the standard sections.
      const keys = sections.map((s) => s.key);
      assert.deepEqual(keys.slice(0, labels.length), labels, `${d.name}: the firmware's order`);
      const rest = keys.slice(labels.length).map((k) => order.indexOf(k));
      assert.ok(rest.every((i) => i >= 0), `${d.name}: the rest are standard sections`);
      assert.deepEqual(rest, [...rest].sort((a, b) => a - b), `${d.name}: the rest in order`);
    }
  }
});

// A route made from the parameter's side - the CV button beside a control -
// is the route a drag on the canvas makes, and the module takes it.
test('a route from the parameter side is the route a drag makes, and moves rather than doubles', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const caps = device.capabilities;
  const patch = codec.emptyPatch();
  const lfo = device.algorithms.find((d) => d?.name === 'LFO');
  const transpose = device.algorithms.find((d) => d?.name === 'Transpose');
  for (const d of [lfo, transpose]) patched(device, patch, d);
  const semitones = transpose.params[0].fields.findIndex((pd) => pd.name === 'semitones');
  assert.ok(semitones >= 0, 'Transpose has a semitones parameter');
  const [cvBus] = patch.nodes[0].outBuses[0];
  assert.notEqual(cvBus, undefined, 'the LFO arrived on a CV bus');

  const fromButton = planBusModulation(patch, caps, 1, semitones, cvBus, { device });
  assert.ok(fromButton.ok, fromButton.why);
  const blocks = patchBlocks(device, patch);
  const fromDrag = planModulation(blocks, patch, caps, { blockId: 'node:0', at: 0, isOutlet: true },
                                  'node:1', semitones, { device });
  assert.ok(fromDrag.ok, fromDrag.why);
  assert.deepEqual(fromButton.routes[0].route, fromDrag.routes[0].route,
                   'the two gestures build one route');

  patch.modMap[fromButton.routes[0].slot] = fromButton.routes[0].route;
  await device.sendPatch(patch, codec.emptyGlobals());       // the firmware validates it

  // Pointing the same parameter at another bus edits the route it has.
  const moved = planBusModulation(patch, caps, 1, semitones, cvBus + 1, { device });
  assert.ok(moved.ok, moved.why);
  assert.equal(moved.routes[0].slot, fromButton.routes[0].slot, 'the same slot');
  assert.deepEqual(moved.routes[0].route.buses, [cvBus + 1]);
  assert.match(moved.said, /now reads/);

  // And the answer to "what is not possible" is a sentence, not a throw.
  assert.equal(planBusModulation(patch, caps, 1, semitones, 99).ok, false);
  assert.equal(planBusModulation(patch, caps, 7, 0, 0).ok, false);
});

// The question the modulation panel exists to answer: what is this doing to
// the parameter right now, and - when the answer is "nothing" - which of the
// several possible nothings it is. Every number here comes from the module
// (SYSEX_GET_MOD_STATE), so this is also the check that the firmware's own
// account of a route and the meter drawn from it are the same account.
test('a modulation route shows what it is doing, and says why when it is doing nothing', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const lfo = device.algorithms.find((d) => d?.name === 'LFO');
  const transpose = device.algorithms.find((d) => d?.name === 'Transpose');
  for (const d of [lfo, transpose]) patched(device, patch, d);
  const semitones = transpose.params[0].fields.findIndex((pd) => pd.name === 'semitones');
  const [bus] = patch.nodes[0].outBuses[0];
  const plan = planBusModulation(patch, device.capabilities, 1, semitones, bus, { device });
  assert.ok(plan.ok, plan.why);
  const slot = plan.routes[0].slot;
  patch.modMap[slot] = plan.routes[0].route;
  await device.sendPatch(patch, codec.emptyGlobals());
  module.advance(400_000);                 // long enough for the LFO to have moved

  const app = fakeApp({ patch, device, module, globals: codec.emptyGlobals() });
  const shown = async () => {
    const state = await device.getModState(slot);
    assert.ok(state, 'the module answers with a state');
    app.session.modLive.set(slot, state);
    const card = NodeCard(app, 1);
    document.body.append(card);
    // The meter is painted per frame from what the module last reported.
    app.live.tick({ module, activity: module.takeActivity() });
    return { state, said: words(card) };
  };

  await withDom(async () => {
    const live = await shown();
    assert.equal(live.state.status, P.ModStatus.MOD_STATUS_ACTIVE, 'a route with a signal on it is active');
    assert.ok(live.state.rangeHi > live.state.rangeLo, 'the module reports the range it may write');
    assert.match(live.said, /signal \d+ %/, 'the meter says what the signal is doing');
    assert.match(live.said, /now /, 'and the control says where the modulation has taken it');
    assert.equal(document.querySelectorAll('[data-mod-slot]').length, 1, 'one meter, on its slot');

    // Depth zero is the most confusing of the nothings, because every other
    // field of the route still reads as correct.
    patch.modMap[slot] = { ...patch.modMap[slot], depth: 0 };
    await device.setModRoute(slot, patch.modMap[slot]);
    module.advance(20_000);
    document.body.children.length = 0;
    const silent = await shown();
    assert.equal(silent.state.status, P.ModStatus.MOD_STATUS_SILENT);
    assert.match(silent.said, /depth is zero/);

    // And the one the module cannot report, because from the matrix's side a
    // bus nobody writes is a perfectly good signal that happens to be zero.
    const empty = P.N_CV_BUS - 1;
    assert.ok(!patch.nodes.some((n) => n.outBuses.some((set) => set.includes(empty))),
              'a bus with no writer');
    patch.modMap[slot] = { ...patch.modMap[slot], buses: [empty], depth: 255 };
    await device.setModRoute(slot, patch.modMap[slot]);
    module.advance(20_000);
    document.body.children.length = 0;
    const quiet = await shown();
    assert.equal(quiet.state.status, P.ModStatus.MOD_STATUS_ACTIVE, 'the matrix is running it');
    assert.match(quiet.said, new RegExp(`nothing writes CV bus ${empty}`));
  });
});

// --- the key, on the node that plays in it -----------------------------------

// Which algorithms play in the key is the module's answer, not a list here:
// the descriptor carries `reads_key` and the registry message brings it over.
test('the module says which algorithms play in the key', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const named = (name) => device.algorithms.find((d) => d?.name === name);
  for (const name of ['Transpose', 'NoteSequencer', 'Note Quantise', 'Chord', 'Harmony', 'Mirror']) {
    assert.equal(named(name).readsKey, true, `${name} plays in the key`);
  }
  // The clock does not, and neither does the node that *writes* the key.
  for (const name of ['Metronome', 'ClockDiv', 'Key']) {
    assert.equal(named(name).readsKey, false, `${name} does not play in the key`);
  }
});

// The badge names the key, and a cable on a node's root inlet outranks it -
// the rule the firmware itself follows, said in the words a player uses.
test('the key badge names the key, and names the chord a cable roots a node on', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const seq = device.algorithms.find((d) => d?.name === 'NoteSequencer');
  const transpose = device.algorithms.find((d) => d?.name === 'Transpose');

  const sequencer = codec.emptyNode(seq.id);
  sequencer.inBuses[rootInlet(seq)] = [3];               // a cable on the root inlet
  sequencer.outBuses[0] = [1];
  const shifter = codec.emptyNode(transpose.id);
  shifter.inBuses[0] = [1];
  shifter.outBuses[0] = [2];
  patch.nodes.push(sequencer, shifter);

  const globals = { ...codec.emptyGlobals(), root: 2, scale: scaleIdOf('minor') };
  // D minor, not C anything: a key rooted on pitch class zero cannot tell a
  // degree from a pitch class.
  const played = { busNote: () => 67 };              // G4 on the root bus
  const app = fakeApp({ patch, device, module: played, globals });

  // A node with no root inlet is in the key and nothing else.
  const plain = withDom(() => KeyBadge(app, { index: 1 }));
  assert.match(words(plain), /D minor/);
  assert.equal(plain.getAttribute('title'), 'the key: D minor');

  // One with a cable on it is named by what the cable is playing: G minor,
  // which is the fourth degree of D minor and sits one fifth below its tonic.
  const rooted = withDom(() => KeyBadge(app, { index: 0 }));
  assert.match(words(rooted), /Gm/);
  assert.match(words(rooted), /\biv\b/);
  assert.match(rooted.getAttribute('title'),
               /G4: degree 4 of the key, 1 fifth below it on the circle, on the root inlet/);

  // The same chord the Harmony circle would draw, worked out the same way.
  const mask = scaleMaskOf('minor');
  const degree = degreeOf(7, 2, mask);
  assert.equal(degree, 3);
  assert.equal(romanNumeral(degree, triadQuality(scaleTriad(degree, 2, mask), 7)), 'iv');
  assert.equal(fifthsFrom(7, 2), 11);                // one fifth counter-clockwise

  // Nothing has played yet on a patched root: the badge says so rather than
  // falling back to the key, because the key is not what that node is hearing.
  const silent = fakeApp({ patch, device, module: { busNote: () => 0xff }, globals });
  const waiting = withDom(() => KeyBadge(silent, { index: 0 }));
  assert.match(words(waiting), /D minor/);
  assert.match(waiting.getAttribute('title'), /the root inlet has played nothing yet/);
});

// A control that is stored, real, and reaching nothing until another control
// says so. The LFO has two rates and one of them is live at a time.
test('a setting the algorithm is currently ignoring says so', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const lfo = device.algorithms.find((d) => d?.name === 'LFO');
  const node = patched(device, patch, lfo);
  const at = (name) => lfo.params.flatMap((g) => (g.repeat > 1 ? [] : g.fields.map((pd, f) => ({ pd, i: g.first + f }))))
    .find((x) => x.pd.name === name).i;

  const app = fakeApp({ patch, device, module, globals: codec.emptyGlobals() });

  // Free-running: the rate decides the cycle, and the note value does not.
  node.params[at('sync')] = 1;
  let said = withDom(() => words(NodeCard(app, 0)));
  assert.match(said, /free-running: the rate decides the cycle/);
  assert.doesNotMatch(said, /the cycle is locked to the clock/);

  // Locked: the other way round, and the control that decides is in the same
  // section as the two it switches between.
  node.params[at('sync')] = 2;
  said = withDom(() => words(NodeCard(app, 0)));
  assert.match(said, /the cycle is locked to the clock/);
  assert.doesNotMatch(said, /the rate decides the cycle/);
});

// A control signal is a level, so the scope keeps its value per column the
// way it keeps a gate's edge, straight off the bus once a pass.
test('a control signal reaches the scope, and the scope lists it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const lfo = device.algorithms.find((d) => d?.name === 'LFO');
  const node = patched(device, patch, lfo);
  await device.sendPatch(patch, codec.emptyGlobals());
  module.advance(1_500_000);
  const [bus] = node.outBuses[0];
  const trace = module.trace;
  let low = Infinity;
  let high = -Infinity;
  for (let i = 0; i < trace.filled; i++) {
    low = Math.min(low, trace.cv[bus][i]);
    high = Math.max(high, trace.cv[bus][i]);
  }
  assert.ok(high > low, `the LFO never moved on the trace (${low}..${high})`);
  assert.ok(high <= P.CV_FULL && low >= -P.CV_FULL, 'in the bus\'s own units');
  assert.equal(module.cv(bus), trace.cv[bus][(trace.head + trace.len - 1) % trace.len],
               'the newest column is what the bus holds now');

  const rows = scopeRows({ patch, device, scopeAll: false });
  assert.ok(rows.some((row) => row.kind === 'cv' && row.bit === bus), 'the bus the patch writes is a row');
  assert.ok(!rows.some((row) => row.kind === 'cv' && row.bit === bus + 1), 'and a bus nobody uses is not');
  assert.ok(rows.every((row) => row.colour), 'every row carries its colour to the legend');
});

// The roll under a node shows both sides of it, in the shades the buses have
// everywhere else; and a shade never leaves its domain.
test('a node\'s roll lists what it reads and writes, in the domain\'s colour', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [2] };
  const chord = device.algorithms.find((d) => d?.name === 'Chord');
  const node = patched(device, patch, chord);
  node.inBuses[0] = [2];                     // the chord is optional to patch; here it is patched
  const sources = nodeRollSources({ patch, device }, 0);
  assert.equal(sources.filter((s) => s.role === 'in').length, 1, 'the held chord, once');
  assert.equal(sources.filter((s) => s.role === 'out').length, 1, 'the chord it makes, once');
  assert.ok(sources.every((s) => /^hsl\(/.test(s.colour)), 'a shade of the note colour');
  assert.deepEqual(nodeRollSources({ patch, device }, 9), [], 'no node, no roll');

  // Shades of green are green: the hue stays within a quarter turn of the
  // base, so a trace of any gate bus still reads as a gate.
  const hue = (c) => Number(/hsl\((\d+)/.exec(c)[1]);
  for (let i = 0; i < 12; i++) {
    const h = hue(shade('#7bd88f', i));
    assert.ok(Math.abs(h - 130) < 60, `shade ${i} wandered to hue ${h}`);
  }
  assert.equal(shade('not a colour', 1), 'not a colour', 'a colour it cannot read is left alone');
  assert.ok(ICON_NAMES.includes('cv') && ICON_NAMES.includes('cut'), 'the icons the buttons ask for exist');
});

// Every block has its signals, whatever it is: a node's gate and control
// ports are rows of a scope and its note ports sources of a roll, a jack is
// its own level and the bus behind it, and a MIDI port is its cables on one
// side and its buses on the other. Each keeps the key it has on the module
// tab, so a chip pressed under a block puts away the same trace it would
// there.
test('every kind of block lists the signals it carries', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_OUT, buses: [1] };
  patch.gatePorts[1] = { direction: P.GatePortDirection.GATE_PORT_IN, buses: [4] };
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0 | P.MidiPort.mmMIDI_SERIAL_1, channel: 0, buses: [2] };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_1, channel: 0, buses: [3] };
  const ctx = { patch, device };

  // A gate node: what it is clocked by and what it writes, both on the scope.
  const div = device.algorithms.find((d) => d?.name === 'ClockDiv');
  patched(device, patch, div);
  const divAt = patch.nodes.length - 1;
  patch.nodes[divAt].inBuses[0] = [4];
  patch.nodes[divAt].outBuses[0] = [1];
  const gate = blockSignals(ctx, { kind: BlockKind.Node, index: divAt });
  assert.equal(gate.sources.length, 0, 'a divider has no notes');
  assert.deepEqual(gate.rows.map((r) => [r.key, r.role, r.source]),
                   [['gate4', 'in', 'gate'], ['gate1', 'out', 'gate']], 'the bus it reads, then the bus it writes');
  assert.match(gate.rows[0].label, /^reads .* · gate 4$/);
  assert.match(gate.rows[1].label, /^writes .* · gate 1$/);

  // A modulator: a control signal is a curve, in the CV colour.
  const lfo = device.algorithms.find((d) => d?.name === 'LFO');
  patched(device, patch, lfo);
  const lfoAt = patch.nodes.length - 1;
  const cv = blockSignals(ctx, { kind: BlockKind.Node, index: lfoAt });
  assert.equal(cv.rows.length, 1);
  assert.equal(cv.rows[0].kind, 'cv');
  assert.match(cv.rows[0].key, /^cv\d+$/, 'the same key the module tab\'s scope gives that bus');

  // A note sequencer: its notes on a roll, and the gate that advances it on
  // the scope above.
  const seq = device.algorithms.find((d) => d?.name === 'NoteSequencer');
  patched(device, patch, seq);
  const seqAt = patch.nodes.length - 1;
  patch.nodes[seqAt].inBuses[0] = [1];
  patch.nodes[seqAt].outBuses[0] = [3];
  const notes = blockSignals(ctx, { kind: BlockKind.Node, index: seqAt });
  assert.deepEqual(notes.rows.map((r) => r.key), ['gate1'], 'the advance edge');
  assert.deepEqual(notes.sources.map((s) => [s.key, s.role]), [['bus3', 'out']], 'and what it plays');
  assert.deepEqual(nodeRollSources(ctx, seqAt), notes.sources, 'the roll under a node is its note ports');

  // A jack: the level at the jack, then the bus behind it - an output reads
  // its bus, an input writes it.
  const out = blockSignals(ctx, { kind: BlockKind.Jack, index: 0 });
  assert.deepEqual(out.rows.map((r) => [r.key, r.source, r.role]), [['jack0', 'jackOut', 'out'], ['gate1', 'gate', 'in']]);
  const inn = blockSignals(ctx, { kind: BlockKind.Jack, index: 1 });
  assert.deepEqual(inn.rows.map((r) => [r.key, r.source, r.role]), [['jack1', 'jackIn', 'in'], ['gate4', 'gate', 'out']]);
  assert.deepEqual(blockSignals(ctx, { kind: BlockKind.Jack, index: 2 }), { rows: [], sources: [] },
                   'an unused jack carries nothing');

  // A MIDI port: the cables are one side and the buses the other, and the
  // cable side is masked to *its* cables.
  const midiIn = blockSignals(ctx, { kind: BlockKind.MidiIn, index: 0 });
  assert.deepEqual(midiIn.sources.map((s) => [s.key, s.role]), [['in', 'in'], ['bus2', 'out']]);
  assert.equal(midiIn.sources[0].mask, P.MidiPort.mmMIDI_USB_0 | P.MidiPort.mmMIDI_SERIAL_1);
  assert.match(midiIn.sources[0].label, /played in · USB 1, DIN 1/);
  const midiOut = blockSignals(ctx, { kind: BlockKind.MidiOut, index: 0 });
  assert.deepEqual(midiOut.sources.map((s) => [s.key, s.role]), [['bus3', 'in'], ['out', 'out']]);
  assert.equal(midiOut.sources[1].mask, P.MidiPort.mmMIDI_USB_1);
  assert.match(midiOut.sources[1].label, /sent out · USB 2/);

  // The panel: a scope for the rows, a roll for the sources, one legend for
  // both, and a chip pressed puts that trace away for this block only.
  const app = fakeApp({ patch, device, module });
  let panel = withDom(() => BlockSignalsPanel(app, { kind: BlockKind.Node, index: seqAt }));
  assert.equal(findAll(panel, (n) => n.tag === 'canvas').length, 2, 'a scope and a roll');
  const chips = findAll(panel, (n) => n.classList?.contains('legend-chip'));
  assert.equal(chips.length, 2);
  chips[1].fire('click');
  assert.ok(app.state.ui.traceHidden.has(`node:${seqAt}:bus3`), 'hidden under this block');
  panel = withDom(() => BlockSignalsPanel(app, { kind: BlockKind.Node, index: seqAt }));
  assert.equal(findAll(panel, (n) => n.tag === 'canvas').length, 1, 'the roll is put away, the scope stays');
  assert.equal(findAll(withDom(() => BlockSignalsPanel(app, { kind: BlockKind.MidiOut, index: 0 })),
                       (n) => n.tag === 'canvas').length, 1,
               'the same bus under another block is still shown');
  assert.match(words(withDom(() => BlockSignalsPanel(app, { kind: BlockKind.Jack, index: 2 }))), /on no bus/,
               'a block on no bus says so');
  assert.equal(BlockSignalsPanel(fakeApp({ patch, device }), { kind: BlockKind.Node, index: seqAt }), null,
               'no module in the page, no signals to draw');
});

// --- the circle of fifths ----------------------------------------------------
//
// Harmony's claim is that the progression is computed from the scale, and the
// circle is where that claim is legible. The picture is drawn from the running
// node's own weights, so this test drives the real module: a metronome
// advancing a harmony with a four-chord loop, and then the card it builds.

// A parameter's index and its enum options, by name, off the descriptor - the
// same way the app finds anything about an algorithm.
function paramNamed(d, name) {
  for (const group of d.params ?? []) {
    for (let f = 0; f < group.nFields; f++) {
      if (group.fields[f]?.name === name) return { at: group.first + f, pd: group.fields[f] };
    }
  }
  return null;
}

test('a harmony draws its key on the circle of fifths, and the loop it wrote', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const harmony = device.algorithms.find((d) => d?.name === 'Harmony');
  const metronome = device.algorithms.find((d) => d?.name === 'Metronome');
  const patch = codec.emptyPatch();

  const clock = patched(device, patch, metronome);

  const node = patched(device, patch, harmony);
  const loop = paramNamed(harmony, 'loop');
  assert.ok(loop.pd.max > 1, 'loop is a length, not a switch: that is the control it needed');
  node.params[loop.at] = 4;
  node.params[paramNamed(harmony, 'seed').at] = 7;     // one seed is one progression
  patch.nodes.push(node);

  // D major, not C: a key whose tonic is not pitch class zero is the only one
  // that can tell a real pitch class from a semitone above the tonic.
  const globals = codec.emptyGlobals();
  globals.scale = P.ScaleId.SCALE_MAJOR;
  globals.root = 2;
  await device.sendPatch(patch, globals);

  // The key is on the app as well as in the module: the circle names its
  // chords the way this key spells them, and that is the app's own table.
  const app = fakeApp({ patch, device, module, globals });
  const harmonyAt = patch.nodes.length - 1;

  // Before it has played: seven chords of C major on the circle, the walk's
  // arrows from the tonic, and four empty slots waiting to be written.
  let card = withDom(() => NodeCard(app, harmonyAt));
  let said = words(card);
  assert.match(said, /circle of fifths/);
  assert.match(said, /7 chords/, 'a major key has seven chords');
  assert.match(said, /circle of fifths · D/, 'the title names the key it drew');
  for (const name of ['D', 'Em', 'F#m', 'G', 'A', 'Bm', 'C#°']) {
    assert.ok(said.split(/\s+/).includes(name), `${name} is not on the circle`);
  }
  for (const roman of ['I', 'ii', 'iii', 'IV', 'V', 'vi', 'vii°']) {
    assert.ok(said.split(/\s+/).includes(roman), `${roman} is not on the circle`);
  }
  assert.match(said, /loop \(4\)/, 'the loop says how long it is');
  // The sentence under the picture is written straight onto the element, the
  // way the frame loop rewrites it as the music moves.
  const caption = (built) => find(built, (kid) => /harmony-caption/.test(kid.className ?? ''))?.textContent ?? '';
  assert.match(caption(card), /writing it down: 0 of 4 chords/, 'nothing is written before it plays');

  // Playing: the loop fills, and the circle draws it as a path through the
  // chords. Long enough for more than four bars of the metronome's default.
  module.advance(20_000_000);
  assert.equal(module.harmonyLoopLength(harmonyAt), 4);
  const written = [0, 1, 2, 3].map((slot) => module.harmonyLoopChord(harmonyAt, slot));
  assert.ok(written.every((d) => d !== NO_STEP), `the loop never filled: ${written}`);
  assert.equal(written[0], 0, 'the first advance is the tonic, so the loop starts on it');
  assert.ok(module.harmonyLoopPosition(harmonyAt) < 4, 'and it is somewhere inside the loop');

  card = withDom(() => NodeCard(app, harmonyAt));
  assert.doesNotMatch(caption(card), /writing it down/, 'the loop is written, not being written');
  assert.match(caption(card), / → /, 'the written loop is named in order');

  // The weights are the firmware's, and they are the circle of fifths: from
  // the tonic of a major key the strongest move is a fifth away. Nothing is
  // ever weighted to zero, so every chord of the key is reachable.
  const weights = [0, 1, 2, 3, 4, 5, 6].map((to) => module.harmonyWeight(harmonyAt, 0, to));
  const best = weights.indexOf(Math.max(...weights));
  assert.ok(best === 3 || best === 4, `the likeliest move from I was degree ${best}, not a fifth`);
  assert.ok(weights.filter((w, d) => d !== 0 && w > 0).length === 6, 'a move the walk can never make');

  // A quality is read off the pitch classes the firmware reports, because
  // nothing in Harmony knows what a chord quality is.
  assert.equal(triadQuality(module.harmonyTriad(harmonyAt, 0), module.harmonyPitch(harmonyAt, 0) % 12), 'maj');
  assert.equal(triadQuality(module.harmonyTriad(harmonyAt, 1), module.harmonyPitch(harmonyAt, 1) % 12), 'min');
  assert.equal(triadQuality(module.harmonyTriad(harmonyAt, 6), module.harmonyPitch(harmonyAt, 6) % 12), 'dim');
  // The triad is pitch classes, so it carries the root that will sound.
  for (let d = 0; d < 7; d++) {
    const set = module.harmonyTriad(harmonyAt, d);
    assert.ok(set & (1 << (module.harmonyPitch(harmonyAt, d) % 12)),
              `degree ${d}: the triad does not contain the root it plays`);
  }

  // Every other algorithm has no circle, which is also how the view knows not
  // to draw one.
  assert.equal(module.harmonyDegrees(0), 0, 'a metronome is not a harmony');
  assert.doesNotMatch(words(withDom(() => NodeCard(app, 0))), /circle of fifths/);

  // **A flat key is spelled flat.** In C minor the third degree is E flat,
  // and calling it "D#" does not read as a spelling slip - it reads as the
  // degrees being wrong, because D is the second and that chord is on the
  // third. The numerals were always right; the names were not.
  globals.scale = P.ScaleId.SCALE_NATURAL_MINOR;
  globals.root = 0;
  await device.sendPatch(patch, globals);
  module.advance(1_000_000);
  card = withDom(() => NodeCard(app, harmonyAt));
  said = words(card);
  for (const name of ['Cm', 'D\u00b0', 'E\u266d', 'Fm', 'Gm', 'A\u266d', 'B\u266d']) {
    assert.ok(said.split(/\s+/).includes(name), `${name} is not on the circle of C minor`);
  }
  for (const wrong of ['D#', 'G#', 'A#']) {
    assert.ok(!said.split(/\s+/).includes(wrong), `${wrong} is a sharp in a flat key`);
  }
  // And the second degree is still the second: this was never the bug, and it
  // has to stay true while the names move.
  assert.equal(module.harmonyPitch(harmonyAt, 1) % 12, 2, 'D is the second degree of C minor');
  assert.equal(module.harmonyPitch(harmonyAt, 2) % 12, 3, 'E flat is the third');
});

// **The picture and the jack have to agree.** The loop's chips mark one chord
// as playing, and the firmware used to be asked for the slot the *next*
// advance would fall on - so the chip lit up was the chord after the one
// coming out of the MIDI jack, every bar, for as long as it ran. The rule now
// lives in one place (app/src/playhead.js) and reads the slot that is
// sounding, so this drives the real module and checks the two against each
// other at every sample.
//
// `shift` is the other half: a loop the walk wrote well but started in the
// wrong place, turned round without drawing another one.
await test('the chip lit is the chord sounding, and shift turns the loop round', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const harmony = device.algorithms.find((d) => d?.name === 'Harmony');
  const metronome = device.algorithms.find((d) => d?.name === 'Metronome');
  const patch = codec.emptyPatch();

  const clock = patched(device, patch, metronome);

  const node = patched(device, patch, harmony);
  node.params[paramNamed(harmony, 'loop').at] = 4;
  node.params[paramNamed(harmony, 'seed').at] = 11;
  patch.nodes.push(node);

  const globals = codec.emptyGlobals();
  globals.scale = P.ScaleId.SCALE_MAJOR;
  await device.sendPatch(patch, globals);

  const app = fakeApp({ patch, device, module, globals });
  const at = patch.nodes.length - 1;
  const shift = paramNamed(harmony, 'shift');
  assert.ok(shift, 'Harmony has a start shift');
  assert.equal(shift.pd.min, 0, 'no shift is the default, so it stores as zero');
  assert.ok(paramSections(harmony.params).find((section) => section.label === 'the form')
              ?.params.some((p) => p.pd.name === 'shift'),
            'shift belongs beside the length it shifts');

  await withDom(async () => {
    document.body.append(NodeCard(app, at));
    const slots = [0, 1, 2, 3];
    const chips = document.body.querySelectorAll('.chord-slot');
    assert.equal(chips.length, 4, 'a chip per slot of the loop');
    const chip = (slot) => chips[slot];
    const lit = () => slots.filter((slot) => chip(slot).classList.contains('playing'));
    const paint = () => app.live.tick({ module, activity: module.takeActivity() });

    // Twenty seconds of the metronome's default, sampled every quarter of a
    // second: the invariant has to hold between chords as well as on them.
    const seen = new Set();
    for (let i = 0; i < 80; i++) {
      module.advance(250_000);
      paint();
      const on = lit();
      const degree = module.harmonyDegree(at);
      if (module.harmonyLoopChord(at, 0) === NO_STEP) continue;   // still writing it down
      assert.equal(on.length, 1, `${on.length} chips lit at sample ${i}`);
      assert.equal(module.harmonyLoopChord(at, on[0]), degree,
                   `the chip lit holds chord ${module.harmonyLoopChord(at, on[0])}, the jack is playing ${degree}`);
      seen.add(on[0]);
    }
    assert.equal(seen.size, 4, `the playhead only ever lit ${[...seen]}`);

    // Turned round: the same four chords, starting two in. The chips are the
    // piece as it will be played, so they move with it.
    const written = slots.map((slot) => module.harmonyLoopChord(at, slot));
    await device.setParam(at, shift.at, 2);
    paint();
    for (const slot of slots) {
      assert.equal(module.harmonyLoopChord(at, slot), written[(slot + 2) % 4],
                   `slot ${slot} did not turn`);
      assert.equal(chip(slot).getAttribute('data-degree'), String(written[(slot + 2) % 4]),
                   `the chip for slot ${slot} was not repainted`);
    }
    // And it is still the chord sounding that is lit.
    for (let i = 0; i < 40; i++) {
      module.advance(250_000);
      paint();
      const on = lit();
      assert.equal(on.length, 1, `${on.length} chips lit after the shift`);
      assert.equal(module.harmonyLoopChord(at, on[0]), module.harmonyDegree(at),
                   'the shift moved the picture off the music');
    }
  });
});

// A control the firmware is currently ignoring says so where it is, because a
// knob that moves and changes nothing is the most confusing thing a module
// can offer - and the reason is never in the parameter itself.
test('a control the key has made inert says so, and is still a control', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const harmony = device.algorithms.find((d) => d?.name === 'Harmony');
  const patch = codec.emptyPatch();
  const node = patched(device, patch, harmony);
  const globals = codec.emptyGlobals();
  globals.scale = P.ScaleId.SCALE_MAJOR;
  globals.root = 0;
  await device.sendPatch(patch, globals);

  const app = fakeApp({ patch, device, module, globals });
  const said = () => words(withDom(() => NodeCard(app, 0)));

  // A major key has a semitone below the tonic, so `leading` is a real
  // control and nothing is said about it.
  assert.doesNotMatch(said(), /no leading tone/, 'a major key has one');

  // A natural minor has not, and the firmware weights nothing with it
  // (src/midi/root_motion.h). The card says so rather than leaving a knob
  // that sweeps and changes nothing.
  globals.scale = P.ScaleId.SCALE_NATURAL_MINOR;
  await device.sendPatch(patch, globals);
  assert.match(said(), /this key has no leading tone/, 'an inert control says why');

  // Dimmed, never disabled: the setting is real and it will do something
  // again the moment the key says so.
  const control = find(withDom(() => NodeCard(app, 0)),
                       (kid) => /(^|\s)param(\s|$)/.test(kid.className ?? '')
                             && /(^|\s)inert(\s|$)/.test(kid.className ?? ''));
  assert.ok(control, 'the row is marked');
  assert.ok(find(control, (kid) => kid.tag === 'input' || kid.tag === 'select'),
            'and still has a control in it, not a disabled one');

  // `drift` redraws a chord of a running loop, so with nothing looping it is
  // inert too - and setting a loop length brings it back.
  assert.match(said(), /nothing is looping/);
  node.params[paramNamed(harmony, 'loop').at] = 4;
  await device.sendPatch(patch, globals);
  assert.doesNotMatch(said(), /nothing is looping/, 'a loop gives drift something to do');
});

// The same argument for the other walk: `deviation` is the chance of leaving
// the cycle and `free` has no cycle, `diatonic` refuses a triad outside the
// key and a chromatic key has no outside.
test("Tonnetz says which of its controls the walk is ignoring", async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const tonnetz = device.algorithms.find((d) => d?.name === 'Tonnetz');
  const patch = codec.emptyPatch();
  const node = patched(device, patch, tonnetz);
  const globals = codec.emptyGlobals();
  globals.scale = P.ScaleId.SCALE_MAJOR;
  globals.root = 0;
  await device.sendPatch(patch, globals);

  const app = fakeApp({ patch, device, module, globals });
  const said = () => words(withDom(() => NodeCard(app, 0)));

  // The default cycle is LR, which names a transform on every step, so
  // deviating from it means something.
  assert.doesNotMatch(said(), /no cycle to leave/, 'LR has a next transform');
  node.params[paramNamed(tonnetz, 'cycle').at] = 4;                 // free
  await device.sendPatch(patch, globals);
  assert.match(said(), /the free walk has no cycle to leave/);

  // A major key leaves triads outside itself for `diatonic` to refuse; a
  // chromatic one does not.
  assert.doesNotMatch(said(), /contains every triad/, 'a major key does not');
  globals.scale = P.ScaleId.SCALE_CHROMATIC;
  await device.sendPatch(patch, globals);
  assert.match(said(), /a chromatic key contains every triad/);
});

// One mechanism, one heading. The editor sorts parameters by what their names
// sound like unless the algorithm says otherwise, and `cycle` sounds like
// behaviour while `deviation` sounds like chance.
test("Tonnetz's walk is shown as one section, not scattered by name", async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const tonnetz = device.algorithms.find((d) => d?.name === 'Tonnetz');
  const sections = paramSections(tonnetz.params);
  const walk = sections.find((s) => s.label === 'the walk');
  assert.ok(walk, 'the walk is a section of its own');
  assert.deepEqual(walk.params.map(({ pd }) => pd.name),
                   ['cycle', 'deviation', 'diatonic', 'seed']);
  const output = sections.find((s) => s.label === 'the output');
  assert.deepEqual(output.params.map(({ pd }) => pd.name), ['octave', 'velocity', 'channel']);
});

// --- the Euclidean ring -------------------------------------------------------
//
// A Euclidean sequencer's pattern is not in its parameters: they say k, n and
// a rotation, and Bjorklund turns them into steps inside the node. The ring is
// the only view of it, so what it has to get right is that it draws the
// *node's* pattern and not a second Bjorklund in JavaScript - checked here
// against the reference tables euclid.h is written to agree with.

test('the Euclidean ring is the pattern the node derived, with the hand on the step sounding', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const euclid = device.algorithms.find((d) => d?.name === 'EuclidianSequencer');
  const metronome = device.algorithms.find((d) => d?.name === 'Metronome');
  assert.ok(euclid && metronome, 'both algorithms are in this firmware');

  const patch = codec.emptyPatch();
  patched(device, patch, metronome);                 // something to advance it
  const node = patched(device, patch, euclid);
  const length = paramNamed(euclid, 'length');
  const pulses = paramNamed(euclid, 'pulses');
  const rotation = paramNamed(euclid, 'rotation');
  node.params[length.at] = 8;
  node.params[pulses.at] = 3;
  await device.sendPatch(patch, codec.emptyGlobals());

  const app = fakeApp({ patch, device, module });
  const at = patch.nodes.length - 1;

  await withDom(async () => {
    document.body.append(NodeCard(app, at));
    const dots = document.body.querySelectorAll('.step-dot');
    const bits = () => [...document.body.querySelectorAll('.step-dot')]
      .map((dot) => (dot.classList.contains('on') ? '1' : '0')).join('');
    const paint = () => app.live.tick({ module, activity: module.takeActivity() });

    assert.equal(dots.length, 8, 'a dot per step of the ring');
    // E(3,8) as Bjorklund published it. A one-line approximation gives a
    // rotation of this, which is exactly the drift the ring must not add.
    assert.equal(bits(), '10010010', 'the ring is E(3,8)');
    assert.equal(document.body.querySelector('.euclid-bits').textContent, '10010010',
                 'and it says so in words');
    assert.match(words(document.body.querySelector('.euclid-caption')), /3 pulses over 8 steps/);
    assert.match(words(document.body.querySelector('.euclid-caption')), /gaps\s+3 3 2/,
                 'the inter-onset intervals, which is how the rhythm is counted');

    // Rotation turns the necklace under a downbeat that does not move, so the
    // picture is the node's rotated pattern - not the same picture spun round.
    await device.setParam(at, rotation.at, 1);
    paint();
    assert.equal(bits(), '00100101', 'the ring followed the rotation');

    // Pulses and length are the other two knobs, and neither rebuilds the card.
    await device.setParam(at, pulses.at, 5);
    await device.setParam(at, length.at, 16);
    paint();
    assert.equal(document.body.querySelectorAll('.step-dot').length, 16, 'the ring grew with the length');
    assert.equal(bits().split('1').length - 1, 5, 'five pulses over sixteen steps');

    // The hand and the lit dot are the step sounding, checked against the node
    // every quarter of a second rather than once.
    const lit = () => [...document.body.querySelectorAll('.step-dot')]
      .flatMap((dot, step) => (dot.classList.contains('playing') ? [step] : []));
    assert.equal(lit().length, 0, 'nothing is lit before the first advance');
    assert.equal(document.body.querySelector('.hand').getAttribute('opacity'), '0',
                 'and the hand is not pointing anywhere yet');

    const seen = new Set();
    for (let i = 0; i < 60; i++) {
      module.advance(250_000);
      paint();
      const on = lit();
      const step = module.seqPosition(at, 0);
      if (step === NO_STEP) continue;
      assert.deepEqual(on, [step], `the ring lit ${on} while the node was on step ${step}`);
      seen.add(step);
    }
    assert.ok(seen.size > 1, `the hand never moved: it sat on ${[...seen]}`);
    assert.equal(document.body.querySelector('.hand').getAttribute('opacity'), '1',
                 'the hand points at the step once there is one');

    // The state a node added from the add bar is in: `pulses` defaults to
    // zero, so the empty ring is the first one anybody sees and it has to say
    // what is missing rather than count gaps that are not there.
    await device.setParam(at, pulses.at, 0);
    paint();
    assert.equal(bits(), '0'.repeat(16), 'no pulses is an empty ring');
    assert.equal(document.body.querySelector('.necklace'), null, 'and no necklace joining nothing');
    assert.match(words(document.body.querySelector('.euclid-caption')), /no pulses over 16 steps/);
    assert.doesNotMatch(words(document.body.querySelector('.euclid-caption')), /gaps/,
                        'a silent ring has no gaps to count');
  });
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
  'minimum', 'maximum', 'minItems', 'maxItems', 'uniqueItems', 'minLength', 'maxLength',
  'prefixItems', 'items',
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
  if (typeof value === 'string') {
    if (schema.minLength !== undefined && value.length < schema.minLength) {
      bad(`is shorter than ${schema.minLength}`);
    }
    if (schema.maxLength !== undefined && value.length > schema.maxLength) {
      bad(`is longer than ${schema.maxLength}`);
    }
  }
  if (Array.isArray(value)) {
    if (schema.minItems !== undefined && value.length < schema.minItems) bad(`needs ${schema.minItems} entries`);
    if (schema.maxItems !== undefined && value.length > schema.maxItems) bad(`holds at most ${schema.maxItems}`);
    if (schema.uniqueItems && new Set(value.map((v) => JSON.stringify(v))).size !== value.length) {
      bad('repeats an entry');
    }
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
test('every example patch passes the schema read from the module', async () => {
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
test('the schema refuses what the firmware refuses', async () => {
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
          { midi_in: [{ port: 1, sources: ['DIN 9'], channel: 0, bus: 0 }] });

  refuses('a macro name longer than the field the patch holds',
          { macros: [{ index: 0, name: 'x'.repeat(caps.macroNameBytes + 1) }] });
  refuses('a macro this module has not got',
          { macros: [{ index: caps.macros, name: 'nope' }] });
  refuses('a macro destination reaching a macro',
          { macros: [{ index: 0, name: 'loop' }],
            macro_dest: [{ slot: 0, macro: 0, targetKind: P.CcTargetKind.CC_TARGET_MACRO, targetIndex: 0 }] });
  refuses('a macro destination pressing the transport',
          { macros: [{ index: 0, name: 'go' }],
            macro_dest: [{ slot: 0, macro: 0, targetKind: P.CcTargetKind.CC_TARGET_TRANSPORT, param: 0 }] });

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
test('a MIDI port is read however it is spelled', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const mask = (name) => fromPatchJson(
    { midi_in: [{ port: 1, sources: [name], channel: 0, bus: 0 }] }, device).patch.midiIn[0].sourceMask;
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
test('every algorithm the sequencer sugar names is one the module has', async () => {
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
test('the prompt carries the schema, the example and this module\'s shape', async () => {
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

// The schema is the bulk of the prompt, and a prompt is paid for by the token.
// Indenting it more than doubles it for a reader that does not exist: what a
// person reads about an algorithm is the panel on the patch tab. So this is
// what stops it being pretty-printed again by someone being helpful.
test('the schema is handed over with no whitespace in it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const text = schemaText(device);
  assert.equal(text.trimEnd().includes('\n'), false, 'the schema is being written a line at a time');
  assert.equal(text.includes('": '), false, 'the schema is being written with spaces after its colons');
  assert.deepEqual(JSON.parse(text), patchSchema(device), 'the text is not the schema');
  // The saving is the point, so it is measured rather than assumed.
  const indented = JSON.stringify(patchSchema(device), null, 2);
  assert.ok(text.length < indented.length * 0.6,
            `whitespace is still ${Math.round((1 - text.length / indented.length) * 100)}% of it`);
});

// Said once.
//
// Thirty-six algorithms share a small vocabulary - a time in milliseconds, a
// MIDI channel, a scale, a step direction - and the conventions of the format
// ("0 means the default", "a null is not connected") are true of every one of
// them. Written out at each use, that repetition was two thirds of the schema
// and told a reader nothing it had not already been told. So: anything that
// turns up twice lives in `$defs` and is referred to, and the rules that are
// true everywhere are on the node schema rather than beside every socket.
test('the schema says each thing once', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const schema = patchSchema(device);

  // Every reference lands on something. A `$ref` that does not resolve is a
  // hole in the schema that no patch would ever reveal.
  const refs = [];
  const walk = (x) => {
    if (Array.isArray(x)) return x.forEach(walk);
    if (!x || typeof x !== 'object') return;
    if (typeof x.$ref === 'string') refs.push(x.$ref);
    Object.values(x).forEach(walk);
  };
  walk(schema);
  assert.ok(refs.length > 100, `only ${refs.length} references: nothing is being shared`);
  for (const ref of new Set(refs)) {
    const target = ref.replace(/^#\//, '').split('/').reduce((o, key) => o?.[key], schema);
    assert.ok(target, `${ref} points at nothing`);
  }

  // No two parameters are described twice. A parameter's *name* is its own and
  // stays at the use site; everything else about a shape used more than once
  // is behind a reference.
  const bodies = new Map();
  for (const branch of schema.properties.nodes.items.allOf) {
    for (const entry of branch.then.properties.params?.prefixItems ?? []) {
      if (entry.$ref) continue;
      const { title, ...body } = entry;
      const key = JSON.stringify(body);
      const before = bodies.get(key);
      assert.ok(!before, `${branch.if.properties.algo.const} "${title}" spells out what `
                       + `${before} already did: ${key}`);
      bodies.set(key, `${branch.if.properties.algo.const} "${title}"`);
    }
  }

  // And the size, which is the thing anyone pasting this actually feels. Per
  // algorithm rather than in total, so a firmware that grows does not fail
  // this - only one that starts repeating itself again does.
  const algorithms = device.algorithms.filter(Boolean).length;
  const text = schemaText(device);
  assert.ok(text.length < algorithms * 2000,
            `${Math.round(text.length / algorithms)} bytes per algorithm, over a 2000 budget`);
});

// The page itself. There is no browser here, so what is checked is that it
// builds from a device and says the two things a first visit needs: that this
// is where the prompt is, and - with no module - why there is nothing to copy.
test('the schema tab builds, and says so when there is no module', async () => {
  const { module } = await instantiate();
  const device = await connected(module);

  const empty = withDom(() => SchemaTab(fakeApp()));
  assert.match(words(empty), /none is attached/, 'a page with no module has to say why it is empty');

  const app = fakeApp({ device });
  app.patches = { patchJson: () => JSON.stringify({ nodes: [{ algo: 'NOT', in: [0], out: [1] }] }, null, 2) };
  const page = withDom(() => SchemaTab(app));
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
test('a slider ignores a scrolling finger and obeys a deliberate one', async () => {
  const made = () => {
    const seen = [];
    const range = withDom(() => Slider(
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

// --- the patch as blocks and arrows ------------------------------------------
//
// The canvas draws a patch; it does not hold one. So what is checked here is
// the part that could be wrong in a way a screenshot would not show: that the
// arrows are the buses, that a drag between two sockets produces a patch the
// firmware accepts, and that a drag it would refuse is refused before it is
// made rather than after.

const idOf = (name, device) => device.algorithms.find((d) => d && d.name === name).id;

// Where an algorithm keeps its channel, found by the name the firmware
// publishes rather than by a number copied out of a .cpp - so this checks the
// descriptors the app actually reads.
function channelParam(device, algorithmId) {
  const descriptor = device.byId.get(algorithmId);
  for (const group of descriptor.params ?? []) {
    if (!group) continue;
    for (let f = 0; f < group.nFields; f++) {
      if (group.fields[f]?.name === 'channel') return group.first + f;
    }
  }
  return -1;
}

// A drag, end to end: plan it from the patch as it is, then write it. This is
// what `App.applyPlan` does, minus the messages.
function dragged(device, patch, from, to) {
  const plan = planConnection(patchBlocks(device, patch), device.capabilities, from, to);
  if (plan.ok) for (const write of plan.writes) applyWrite(patch, write);
  return plan;
}

test('the arrows are the buses, not a second model of the patch', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patched(device, patch, idOf('ClockDiv', device));
  patched(device, patch, idOf('StepSequencer', device));

  const blocks = patchBlocks(device, patch);
  assert.equal(blocks.length, 2);
  assert.equal(blocks[0].title, 'ClockDiv');
  assert.equal(blocks[1].inlets[0].required, true, 'the advance inlet must be connected');

  const arrows = connectionsOf(blocks);
  assert.equal(arrows.length, 1, 'the sequencer reads the divider');
  assert.equal(arrows[0].from.blockId, 'node:0');
  assert.equal(arrows[0].to.blockId, 'node:1');
  assert.deepEqual([arrows[0].bus], patch.nodes[0].outBuses[0]);

  // Take the outlet off its bus and the arrow is gone: there was never
  // anything else holding it there.
  patch.nodes[0].outBuses[0] = [];
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0);
});

test('a jack and a MIDI port are blocks with one socket each', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_IN, buses: [2] };
  patch.gatePorts[1] = { direction: P.GatePortDirection.GATE_PORT_OUT, buses: [2] };
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [1] };
  patch.midiOut[3] = { targetMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 0, buses: [1] };

  const blocks = patchBlocks(device, patch);
  assert.deepEqual(blocks.map((b) => b.id), ['midiIn:0', 'jack:0', 'jack:1', 'midiOut:3'],
                   'what arrives, then what the patch does, then what leaves');
  assert.equal(blocks[1].outlets.length, 1, 'a jack in drives a bus');
  assert.equal(blocks[2].inlets.length, 1, 'a jack out is driven by one');

  const arrows = connectionsOf(blocks);
  assert.equal(arrows.length, 2, 'jack 1 to jack 2, and MIDI in 1 to MIDI out 4');

  // An unused jack is not a block: eight empty boxes around every patch is not
  // a drawing of it.
  patch.gatePorts[1] = { direction: P.GatePortDirection.GATE_PORT_UNUSED, buses: [] };
  assert.equal(patchBlocks(device, patch).length, 3);
});

test('a drag from an outlet to an inlet is a patch the firmware takes', async () => {
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
  assert.deepEqual(patch.nodes[0].outBuses[0], patch.nodes[1].inBuses[0]);

  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, codec.emptyGlobals());     // throws if it is refused
});

test('one outlet, two readers, one bus', async () => {
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
  assert.deepEqual(patch.nodes[1].inBuses[0], patch.nodes[2].inBuses[0], 'both read the same bus');

  const arrows = connectionsOf(patchBlocks(device, patch));
  assert.equal(arrows.length, 2);
  assert.deepEqual(arrows.map((a) => a.readers), [2, 2], 'both arrows know the bus is shared');
  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, codec.emptyGlobals());
});

test('a drag the module would refuse is refused before it is made', async () => {
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
  assert.deepEqual(patch.nodes[0].outBuses[0], []);
  assert.deepEqual(patch.nodes[1].inBuses[0], []);
});

test('which end the drag started at does not change what it connects', async () => {
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

test('disconnecting one arrow leaves the inlet\'s other sources alone', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
  patch.nodes.push(codec.emptyNode(idOf('Metronome', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
  // Two sources on buses of their own, merged by the reader: the sequencer's
  // advance inlet listens to both. This is the shape one bus per port could
  // not hold without moving one of the sources onto the other's bus.
  patch.nodes[0].outBuses[0] = [4];
  patch.nodes[1].outBuses[0] = [5];
  patch.nodes[2].inBuses[0] = [4, 5];

  const blocks = patchBlocks(device, patch);
  const arrows = connectionsOf(blocks);
  assert.equal(arrows.length, 2, 'two writers, one reader');
  const plan = planDisconnect(blocks, arrows[0]);
  assert.ok(plan.ok);
  assert.match(plan.said, /still reading bus 5/);
  for (const write of plan.writes) applyWrite(patch, write);
  assert.deepEqual(patch.nodes[2].inBuses[0], [5]);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 1,
               'only the arrow that was cut went; the other source is untouched');

  // And clearing a socket directly takes every arrow at it at once.
  patch.nodes[2].inBuses[0] = [4, 5];
  const cleared = planClear(patchBlocks(device, patch), { blockId: 'node:2', at: 0, isOutlet: false });
  assert.ok(cleared.ok);
  for (const write of cleared.writes) applyWrite(patch, write);
  assert.deepEqual(patch.nodes[2].inBuses[0], []);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0);
});

// The drag that could not be made at all before: a second source into an
// inlet that already has one, with neither source losing what it was already
// driving.
test('a second source dragged into an inlet is summed, not swapped', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));      // gate out
  patch.nodes.push(codec.emptyNode(idOf('Metronome', device)));     // gate out
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device))); // gate in
  patch.nodes.push(codec.emptyNode(idOf('EuclidianSequencer', device)));

  // The divider drives both sequencers; the metronome drives the first as well.
  dragged(device, patch, { blockId: 'node:0', at: 0, isOutlet: true },
                         { blockId: 'node:2', at: 0, isOutlet: false });
  dragged(device, patch, { blockId: 'node:0', at: 0, isOutlet: true },
                         { blockId: 'node:3', at: 0, isOutlet: false });
  const merge = dragged(device, patch, { blockId: 'node:1', at: 0, isOutlet: true },
                                       { blockId: 'node:2', at: 0, isOutlet: false });
  assert.ok(merge.ok, merge.why);
  assert.match(merge.said, /summed with/);

  const divider = patch.nodes[0].outBuses[0][0];
  const metronome = patch.nodes[1].outBuses[0][0];
  assert.notEqual(divider, metronome, 'each source kept a bus of its own');
  assert.deepEqual(patch.nodes[2].inBuses[0], [divider, metronome].sort((a, b) => a - b));
  assert.deepEqual(patch.nodes[3].inBuses[0], [divider], 'the other reader is untouched');

  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, codec.emptyGlobals());
});

test('a free bus is one nothing writes, and running out says so', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  for (let i = 0; i < device.capabilities.gateBuses; i++) {
    const node = codec.emptyNode(idOf('ClockDiv', device));
    node.outBuses[0] = [i];
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

test('a jack and a MIDI port are disconnected like any other port', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  const blocks = patchBlocks(device, patch);
  const arrow = connectionsOf(blocks)[0];

  // A port in use and on no bus is a patch the module takes: it is what every
  // block starts as. So there is nothing special about cutting one.
  const cut = planDisconnect(blocks, arrow);
  assert.ok(cut.ok, cut.why);
  for (const write of cut.writes) applyWrite(patch, write);
  assert.deepEqual(patch.midiOut[0].buses, []);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0);
  assert.deepEqual(validate(device, patch), [], 'in use, wired to nothing, and still legal');

  const cleared = planClear(patchBlocks(device, patch), { blockId: 'midiIn:0', at: 0, isOutlet: true });
  assert.ok(cleared.ok, cleared.why);
  for (const write of cleared.writes) applyWrite(patch, write);
  assert.deepEqual(patch.midiIn[0].buses, []);
  assert.deepEqual(validate(device, patch), []);
});

// --- what a removal takes with it ---------------------------------------------
//
// A bus is the connection, so a block leaving the patch has to let go of both
// ends of every wire it was on. A port still speaking on the bus its old
// destination left behind is a wire nobody can see: it draws no arrow, and it
// is why "delete it and patch it somewhere else" used to land the new wire
// back on the old bus.

test('a MIDI port taken out of use leaves no bus behind, at either end', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const { editor } = editorOn(device, patch);
  editor.addMidiPort(false);
  editor.addMidiPort(true);
  dragged(device, patch, { blockId: 'midiIn:0', at: 0, isOutlet: true },
                         { blockId: 'midiOut:0', at: 0, isOutlet: false });
  assert.deepEqual(patch.midiOut[0].buses, [0]);
  assert.deepEqual(patch.midiIn[0].buses, [0]);

  editor.removeBlock({ kind: 'midiOut', index: 0 });
  assert.equal(patch.midiOut[0].targetMask, 0);
  assert.deepEqual(patch.midiOut[0].buses, [], 'a port out of use is on no bus');
  assert.deepEqual(patch.midiIn[0].buses, [], 'and nothing is left playing to nobody');

  // So the port comes back unconnected, and what it is patched to next
  // decides the bus - rather than the one the last patch left on it.
  editor.addMidiPort(true);
  assert.deepEqual(patch.midiOut[0].buses, []);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0);
  const plan = dragged(device, patch, { blockId: 'midiIn:0', at: 0, isOutlet: true },
                                      { blockId: 'midiOut:0', at: 0, isOutlet: false });
  assert.ok(plan.ok, plan.why);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 1, 'one wire, not two');
  assert.deepEqual(validate(device, patch), []);
});

test('a node removed takes its wires with it, and leaves the shared ones alone', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const { editor } = editorOn(device, patch);
  editor.addMidiPort(false);
  editor.addMidiPort(true);
  editor.addNode(idOf('Transpose', device));
  dragged(device, patch, { blockId: 'midiIn:0', at: 0, isOutlet: true },
                         { blockId: 'node:0', at: 0, isOutlet: false });
  dragged(device, patch, { blockId: 'node:0', at: 0, isOutlet: true },
                         { blockId: 'midiOut:0', at: 0, isOutlet: false });
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 2);
  const [inBus] = patch.nodes[0].inBuses[0];
  const [outBus] = patch.nodes[0].outBuses[0];

  // A second output on the transposer's bus: one of two readers going is a
  // fan-out losing an arrow, not the source going quiet.
  editor.addMidiPort(true);
  dragged(device, patch, { blockId: 'node:0', at: 0, isOutlet: true },
                         { blockId: 'midiOut:1', at: 0, isOutlet: false });
  editor.removeBlock({ kind: 'midiOut', index: 1 });
  assert.deepEqual(patch.nodes[0].outBuses[0], [outBus], 'the other output still reads it');
  assert.deepEqual(patch.midiOut[0].buses, [outBus]);

  editor.removeNode(0);
  assert.equal(patch.nodes.length, 0);
  assert.deepEqual(patch.midiIn[0].buses, [], `nothing writes note bus ${inBus} any more`);
  assert.deepEqual(patch.midiOut[0].buses, [], `nothing reads note bus ${outBus} any more`);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0);
  assert.deepEqual(validate(device, patch), []);
  assert.deepEqual(advise(device, patch), [], 'and no inlet is left listening to silence');
});

test('a MIDI port turned round leaves the port it came from on no bus', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 3, buses: [2] };
  const plan = planPortFlip(patch, device.capabilities, 0, false);
  assert.ok(plan.ok, plan.why);
  applyPortFlip(patch, plan);
  assert.deepEqual(patch.midiOut[plan.to.index].buses, [2], 'what it carried moved with it');
  assert.deepEqual(patch.midiIn[0].buses, [], 'and the port it left is on no bus');
  assert.deepEqual(validate(device, patch), []);
});

test('a port taken into use arrives on no bus at all', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  // A MIDI output on note bus 0 that nothing writes - which `advise` flags,
  // and which adding a MIDI input must *not* silently answer: a source that
  // lands on whatever was waiting is a wire nobody drew.
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [] };
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 0, 'two blocks, no arrow');
  assert.deepEqual(validate(device, patch), []);

  // And a drag is what connects it - to the bus the output is already on,
  // because that is the one the reader is listening to.
  const plan = dragged(device, patch, { blockId: 'midiIn:0', at: 0, isOutlet: true },
                                      { blockId: 'midiOut:0', at: 0, isOutlet: false });
  assert.ok(plan.ok, plan.why);
  assert.equal(connectionsOf(patchBlocks(device, patch)).length, 1);
  assert.deepEqual(validate(device, patch), []);
});

// --- the patch's edges, and the list you add them from ------------------------

// The add list is thirty algorithms. Grouped by what the *module* says each
// one is, it is six short lists - and an algorithm added to the firmware
// arrives on a shelf with no change here, which is the same promise the
// registry has always made about names and summaries.
test('the add list is shelved by what the module says each algorithm is', async () => {
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

// The key has one value, so the module refuses a second Key node. Offering
// one and having it refused on the way out is the worst of both: the list
// stops offering it once the patch holds it.
test('an algorithm there may only be one of is offered once', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const key = device.algorithms.find((d) => d?.name === 'Key');
  assert.ok(key?.singleton, 'the module says the Key node is a singleton');
  assert.ok(!device.algorithms.some((d) => d && d.singleton && d.name !== 'Key'),
            'and it is the only one so far');

  const offered = (inPatch) =>
    optionsOf(catalogue(device.algorithms, [], inPatch)).some((o) => o.label === 'Key');
  assert.ok(offered([]), 'a patch without one can add one');
  assert.ok(!offered([key.id]), 'a patch with one is not offered a second');
  // Everything else is unaffected: one Chord does not stop a second.
  const chord = device.algorithms.find((d) => d?.name === 'Chord');
  assert.ok(optionsOf(catalogue(device.algorithms, [], [chord.id])).some((o) => o.label === 'Chord'));
});

// An algorithm from firmware this app has never heard of still has to be
// offered: the category is appended to the registry record precisely so that
// an unknown one costs a shelf, not an algorithm.
test('an algorithm whose category this app does not know is still offered', () => {
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
test('a jack turned round keeps its bus, and an unused one lands on a real one', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const caps = device.capabilities;
  const patch = codec.emptyPatch();
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_IN, buses: [3] };

  const turned = planJackDirection(patch, caps, 0, P.GatePortDirection.GATE_PORT_OUT);
  assert.deepEqual(turned, { index: 0, direction: P.GatePortDirection.GATE_PORT_OUT, buses: [3] });

  // Off, and the buses go with it: a jack put away lets go of what it was
  // patched to, so putting it back does not silently reconnect it.
  const off = planJackDirection(patch, caps, 0, P.GatePortDirection.GATE_PORT_UNUSED);
  assert.deepEqual(off.buses, []);
  patch.gatePorts[0] = { direction: off.direction, buses: off.buses };
  assert.deepEqual(validate(device, patch), []);

  // And back on: in use, on no bus, waiting to be dragged.
  const on = planJackDirection(patch, caps, 0, P.GatePortDirection.GATE_PORT_IN);
  assert.deepEqual(on.buses, []);
  patch.gatePorts[0] = { direction: on.direction, buses: on.buses };
  assert.deepEqual(validate(device, patch), [], 'the module takes what the toggle wrote');
});

// A MIDI port's direction is the same setting to the eye and a different thing
// underneath: the module has four inputs and four outputs, so the toggle moves
// the port. What it carries has to travel with it, or "turn it round" quietly
// loses the cables and the channel it was set to.
test('a MIDI port turned round takes its cables, channel and bus with it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const caps = device.capabilities;
  const patch = codec.emptyPatch();
  patch.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_SERIAL_1, channel: 5, buses: [2] };

  const plan = planPortFlip(patch, caps, 0, false);
  assert.equal(plan.ok, true);
  assert.deepEqual(plan.to, { index: 0, isOut: true }, 'the first free output takes it');
  applyPortFlip(patch, plan);
  assert.equal(patch.midiIn[0].sourceMask, 0, 'the input it left is unused');
  assert.deepEqual(
    { mask: patch.midiOut[0].targetMask, channel: patch.midiOut[0].channel, buses: patch.midiOut[0].buses },
    { mask: P.MidiPort.mmMIDI_SERIAL_1, channel: 5, buses: [2] });
  assert.deepEqual(validate(device, patch), []);

  // The patch says one MIDI port, pointing the other way - not two.
  const blocks = patchBlocks(device, patch);
  assert.deepEqual(blocks.map((b) => b.id), ['midiOut:0']);

  // And it can refuse: every port on the other side already in use is a
  // failure with a reason, not a silently dropped edit.
  const full = codec.emptyPatch();
  for (let i = 0; i < caps.midiOut; i++) {
    full.midiOut[i] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  }
  full.midiIn[0] = { sourceMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  const refused = planPortFlip(full, caps, 0, false);
  assert.equal(refused.ok, false);
  assert.match(refused.why, /every MIDI output port/);
});

// One source, two destinations. The module has always run this - two MIDI
// outputs naming one note bus is two cables carrying the same music - but the
// only way to build it was to know that a spare port slot was where to go. The
// half that travels is the source, and it is a different half each way round.
test('a MIDI output fans out to a second cable, never to the same one', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const caps = device.capabilities;
  const patch = codec.emptyPatch();
  patch.midiOut[0] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 3, buses: [2] };

  const out = planPortFanOut(patch, caps, 0);
  assert.equal(out.ok, true, out.why);
  assert.deepEqual(out.buses, [2], 'the note buses are the source, so they travel');
  assert.equal(out.channel, 3);
  assert.notEqual(out.mask & P.MidiPort.mmMIDI_USB_0, P.MidiPort.mmMIDI_USB_0,
                  'not the cable it is already playing: that would be every note twice');
  patch.midiOut[out.index] = { targetMask: out.mask, channel: out.channel, buses: out.buses };
  assert.deepEqual(validate(device, patch), []);
  assert.equal(patchBlocks(device, patch).filter((b) => b.kind === 'midiOut').length, 2);
  await device.sendPatch(patch, codec.emptyGlobals());     // throws if it is refused

  // And it refuses with a reason rather than writing over a port in use.
  const full = codec.emptyPatch();
  for (let i = 0; i < caps.midiOut; i++) {
    full.midiOut[i] = { targetMask: P.MidiPort.mmMIDI_USB_0, channel: 0, buses: [0] };
  }
  const refused = planPortFanOut(full, caps, 0);
  assert.equal(refused.ok, false);
  assert.match(refused.why, /every MIDI output port/);
});

// --- where the blocks go -----------------------------------------------------

test('the layout runs the signal left to right, and a loop does not hang it', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patched(device, patch, idOf('ClockDiv', device));
  patched(device, patch, idOf('StepSequencer', device));
  patched(device, patch, idOf('GateToNote', device));

  const blocks = patchBlocks(device, patch);
  const positions = autoLayout(blocks, connectionsOf(blocks));
  const x = blocks.map((b) => positions.get(b.id).x);
  assert.ok(x[0] < x[1] && x[1] < x[2], `a chain goes rightwards, not ${x}`);

  // A sequencer resetting the divider that advances it is a legal patch - a
  // bus is read and written once a pass - and a layout that walked it looking
  // for the furthest-left source would never come back.
  patch.nodes[0].inBuses[0] = [patch.nodes[1].outBuses[0]];
  const looped = patchBlocks(device, patch);
  const round = autoLayout(looped, connectionsOf(looped));
  assert.equal(round.size, looped.length, 'every block still got a position');
});

test('a socket is inside the block it belongs to', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patched(device, patch, idOf('DrumSeqGate', device));       // eight outlets, two inlets
  const block = patchBlocks(device, patch)[0];
  const at = { x: 0, y: 0 };
  const last = block.outlets[block.outlets.length - 1];
  const point = socketPoint(at, last.at, true);
  assert.equal(point.x, BLOCK_W, 'an outlet is on the right edge');
  assert.ok(point.y < blockHeight(block), 'and inside the block, which is as tall as its rows');
  assert.equal(socketPoint(at, 1, true).y - socketPoint(at, 0, true).y, ROW_H);
  assert.equal(socketPoint(at, 0, false).x, 0, 'an inlet is on the left edge');
});

test('a block dragged somewhere is remembered beside the patch, not in it', async () => {
  const library = new Library(fakeStorage());
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(12));
  const doc = toPatchJson(patch, codec.emptyGlobals(), null);
  const entry = library.save({ name: 'arranged', patch: doc, nodes: 1 });

  library.saveLayout(entry.id, { 'node:0': [400, 120] });
  assert.deepEqual(library.layoutFor(entry.id), { 'node:0': [400, 120] });
  // A coordinate is not part of a patch: what the library keeps, what a file
  // carries and what the module stores are untouched by arranging it.
  assert.deepEqual(library.get(entry.id).patch, doc);

  // An unsaved patch keeps its arrangement under the same name the autosave
  // uses, and the arrangement follows it into the library.
  library.saveLayout('working', { 'node:0': [10, 20] });
  library.moveLayout('working', 'p-new');
  assert.equal(library.layoutFor('working'), null);
  assert.deepEqual(library.layoutFor('p-new'), { 'node:0': [10, 20] });

  library.dropLayout(entry.id);
  assert.equal(library.layoutFor(entry.id), null);

  // A patch nobody arranged simply gets the automatic layout.
  assert.equal(library.layoutFor('nobody'), null);
});

test('removing a node moves the blocks after it with it', async () => {
  const saved = { 'node:0': [0, 0], 'node:1': [1, 1], 'node:3': [3, 3], 'jack:2': [9, 9] };
  const after = forgetNode(saved, 1);
  assert.deepEqual(after, { 'node:0': [0, 0], 'node:2': [3, 3], 'jack:2': [9, 9] },
                   'node 3 became node 2, node 1 is gone, the jack did not move');
});

test('a hand-placed block stays where it was put', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patched(device, patch, idOf('ClockDiv', device));
  patched(device, patch, idOf('StepSequencer', device));
  const blocks = patchBlocks(device, patch);
  const arrows = connectionsOf(blocks);
  const placed = layoutOf(blocks, arrows, { 'node:1': [777, 333] });
  assert.deepEqual(placed.get('node:1'), { x: 777, y: 333 });
  assert.deepEqual(placed.get('node:0'), autoLayout(blocks, arrows).get('node:0'),
                   'and nothing else moved because of it');
});

// --- copying a block ---------------------------------------------------------
//
// What a copy is worth is the settings: a sequencer somebody spent ten minutes
// on, wanted twice. What it must not carry is a bus its outlet writes, because
// two writers on one bus is a merge - the copy would change the sound of the
// patch it was copied in, before anybody had drawn a thing.

// An editor over the real module, with the arrangement stubbed: what a test
// reads back is the patch it wrote and where it asked a block to go.
function editorOn(device, patch) {
  const state = createState();
  state.patch = patch;
  const placed = [];
  const editor = new Editor({
    state,
    session: { device, offline: false },
    arrangement: { place: (id, at) => placed.push([id, at]), forgetNode: () => {} },
    render: () => {},
  });
  return { state, editor, placed };
}

test('a copied node carries its settings and what it listens to, and writes nothing', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
  patch.nodes[0].outBuses[0] = [3];
  patch.nodes[1].inBuses[0] = [3];
  patch.nodes[1].outBuses[0] = [5];
  patch.nodes[1].params[0] = 12;

  const { state, editor } = editorOn(device, patch);
  const sequencer = patchBlocks(device, patch).find((b) => b.id === 'node:1');
  editor.copyBlock(sequencer);
  assert.ok(state.ui.canvas.clipboard, 'the clipboard is the page\'s, not the system\'s');
  editor.paste();

  assert.equal(patch.nodes.length, 3);
  const made = patch.nodes[2];
  assert.equal(made.algorithmId, patch.nodes[1].algorithmId);
  assert.deepEqual([...made.params], [...patch.nodes[1].params], 'the settings are the point of a copy');
  assert.deepEqual(made.inBuses[0], [3], 'still advanced by what advanced the original');
  assert.deepEqual(made.outBuses.flat(), [], 'and says nothing until a wire is drawn from it');
  assert.deepEqual(state.ui.canvas.selected, { kind: 'block', id: 'node:2' },
                   'what was just pasted is what the details are showing');
  assert.deepEqual(validate(device, patch), []);
  await device.sendPatch(patch, state.globals);
});

test('a duplicate goes under the block it came from and leaves the clipboard alone', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('ClockDiv', device)));
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));

  const { state, editor, placed } = editorOn(device, patch);
  const blocks = patchBlocks(device, patch);
  editor.copyBlock(blocks[0]);
  const kept = state.ui.canvas.clipboard;

  const positions = layoutOf(blocks, connectionsOf(blocks), {});
  const under = underBlock(positions, blocks[1]);
  assert.equal(under.x, positions.get('node:1').x, 'in the same column');
  assert.ok(under.y > positions.get('node:1').y, 'and below it');

  editor.duplicateBlock(blocks[1], under);
  assert.equal(patch.nodes[2].algorithmId, patch.nodes[1].algorithmId);
  assert.deepEqual(placed.at(-1), ['node:2', under]);
  assert.equal(state.ui.canvas.clipboard, kept, 'duplicating one thing does not lose the copy of another');

  // A jack is not a thing a patch has more of: the add bar takes the next
  // free one into use.
  patch.gatePorts[0] = { direction: P.GatePortDirection.GATE_PORT_IN, buses: [] };
  const jack = patchBlocks(device, patch).find((b) => b.id === 'jack:0');
  editor.duplicateBlock(jack);
  assert.equal(patch.nodes.length, 3, 'nothing was added');
  assert.match(state.status, /add bar/);
});

test('a copy the module could not hold is refused rather than pasted', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patch.nodes.push(codec.emptyNode(idOf('StepSequencer', device)));
  patch.nodes[0].inBuses[0] = [1];

  // A bus this module has not got - the clipboard outlives the patch it came
  // from, and the next module may be smaller.
  const wide = copyNode(patch, 0);
  wide.inBuses[0] = [1, 200];
  assert.deepEqual(nodeFromCopy(device, wide).inBuses[0], [1]);

  // An algorithm it does not run at all is not a node it can make.
  assert.equal(nodeFromCopy(device, { ...wide, algorithmId: 0xfe }), null);
  const { state, editor } = editorOn(device, patch);
  state.ui.canvas.clipboard = { ...wide, algorithmId: 0xfe };
  editor.paste();
  assert.equal(patch.nodes.length, 1);
  assert.match(state.error, /has not got that algorithm/);
});

test('the canvas keys act on what is selected, and only on the canvas', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patched(device, patch, idOf('ClockDiv', device));
  patched(device, patch, idOf('StepSequencer', device));
  const app = fakeApp({ patch, device });
  const press = (key, extra = {}) => canvasKey(app, { key, preventDefault() {}, ...extra });
  const last = () => app.calls.at(-1)?.[0];

  // Nothing selected: the key that removes something removes nothing.
  assert.equal(press('Delete'), 'delete');
  assert.equal(last(), 'say');

  app.state.ui.canvas.selected = { kind: 'block', id: 'node:1' };
  assert.equal(press('c', { metaKey: true }), 'copy');
  assert.equal(last(), 'copyBlock');
  assert.equal(press('d', { ctrlKey: true }), 'duplicate');
  assert.equal(last(), 'duplicateBlock');
  assert.equal(press('v', { metaKey: true }), 'paste');
  assert.equal(last(), 'paste');
  assert.equal(press('Backspace'), 'delete');
  assert.equal(last(), 'removeBlock');

  // An arrow is the one bus its reader stops reading, which is the cut button.
  const arrow = connectionsOf(patchBlocks(device, patch))[0];
  app.state.ui.canvas.selected = { kind: 'arrow', id: arrow.id };
  assert.equal(press('Delete'), 'delete');
  assert.equal(last(), 'applyPlan');
  assert.equal(app.calls.at(-1)[1].ok, true);

  // A key typed into a field is a character; a held key is one press; and
  // another tab is not the canvas.
  assert.equal(press('c', { metaKey: true, target: { tagName: 'INPUT' } }), null);
  assert.equal(press('c', { metaKey: true, repeat: true }), null);
  assert.equal(press('d', { metaKey: true, altKey: true }), null);
  app.state.ui.tab = 'midi';
  assert.equal(press('c', { metaKey: true }), null);
});

test('the canvas takes the whole window, and gives it back', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  patched(device, patch, idOf('ClockDiv', device));
  const app = fakeApp({ patch, device });

  withDom(() => {
    const press = (key) => canvasKey(app, { key, preventDefault() {} });
    assert.equal(press('f'), 'full');
    assert.equal(app.state.ui.canvas.full, true);
    assert.equal(app.state.ui.canvas.fit, true, 'the window changed shape under the patch');

    const panel = CanvasPanel(app, geometry(app));
    assert.ok(String(panel.className).split(/\s+/).includes('full'));
    const paste = find(panel, (n) => String(n.attrs['aria-label'] ?? '').startsWith('paste'));
    assert.ok(paste, 'the paste is a button too: a phone has no keyboard');
    assert.ok(paste.attrs.disabled, 'and is dead while nothing has been copied');

    assert.equal(press('Escape'), 'full');
    assert.equal(app.state.ui.canvas.full, false);
    // Escape with nothing to leave and nothing selected is not the canvas's.
    assert.equal(press('Escape'), null);
  });
});

// --- the drums ---------------------------------------------------------------
//
// The kits themselves cannot be checked here - "does this sound like a 909"
// is not a question a test answers - so what is checked is everything around
// them: that a kit can play every drum, that the drum a lane means is the one
// the firmware means by it, that a patch's drum sequencers are found with the
// buses they speak on, and that a hit is played once, by the sequencer that
// made it, rather than by whichever player happened to carry it.

test('the drum a gate lane plays is the one the firmware names', async () => {
  // A `DrumSeqGate` lane carries no note number at all, so the only thing that
  // says which drum it is, is the note the firmware would send for that lane
  // if this were the MIDI variant. Read it out of the firmware rather than
  // trusting the copy in `drums.js`: a lane renamed there and not here would
  // be a snare on the kick's lane, silently, for ever.
  const source = readFileSync(join(repoRoot, 'src', 'algorithm', 'sequencer', 'drum_sequencer.cpp'), 'utf8');
  const found = /GM_DEFAULT_NOTE\[DRUM_SEQ_LANES\]\s*=\s*\{([^}]*)\}/.exec(source);
  assert.ok(found, 'the firmware no longer spells its default notes out where this can read them');
  const notes = found[1].split(',').map((n) => Number(n.trim()));
  assert.deepEqual(LANE_NOTES, notes, 'the lane notes in drums.js are not the firmware’s');
  assert.equal(notes.length, P.DRUM_SEQ_LANES);
  // And the map from note to drum has to have an opinion about every one of
  // them, or a default lane plays the tuned fallback instead of a drum.
  for (const note of notes) assert.notEqual(pieceOf(note), 'perc', `note ${note} has no drum`);
});

test('every kit can play every drum, and no two kits are the same kit', async () => {
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
  // An unknown kit is a stored setup naming a kit this build does not have,
  // or a selector given a value it should not have: it plays rather than
  // throws.
  assert.ok(voiceSpec('no such kit', 'snare').noise, 'an unknown kit has no fallback');
});

test('a kit builds real audio nodes for every drum in it', async () => {
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

test('the drum sequencers of a patch are found, with the buses they speak on', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const midi = patched(device, patch, idOf('DrumSeqMidi', device));
  const gate = patched(device, patch, idOf('DrumSeqGate', device));
  // Three lanes out, five left alone - the ordinary shape of a drum patch.
  gate.outBuses[0] = [4]; gate.outBuses[1] = [5]; gate.outBuses[2] = [6];

  const sources = drumSources(device, patch);
  assert.equal(sources.length, 2, 'both drum sequencers should be found');
  assert.deepEqual(sources.map((s) => s.key), ['node:0', 'node:1'],
                   'a drum voice is keyed by node index, like everything else addressed');
  assert.equal(sources[0].kind, 'note');
  assert.deepEqual(sources[0].buses, midi.outBuses[0], 'the MIDI variant speaks on its note outlet');
  assert.equal(sources[1].kind, 'gate');
  assert.deepEqual(sources[1].lanes.map((lane) => [lane.buses, lane.piece]),
                   [[[4], 'kick'], [[5], 'snare'], [[6], 'hatClosed']],
                   'a gate lane is the drum the firmware means by that lane');
  assert.equal(sources[1].lanes.length, 3, 'a lane on no bus is not a lane to listen to');

  // A patch with no drums in it has no drum voices to show, and nothing else
  // in the registry may be mistaken for one.
  patched(device, patch, idOf('ClockDiv', device));
  assert.equal(drumSources(device, patch).length, 2);
});

test('a drum note is played by its own sequencer, not by the player that carried it', async () => {
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
    { key: 'node:0', index: 0, label: 'DrumSeqMidi 0', kind: 'note', buses: [3], lanes: [] },
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

test('a node set to channel 10 is a drum machine, whatever algorithm it runs', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const id = idOf('GateToNote', device);
  const node = patched(device, patch, id);
  const at = channelParam(device, id);
  assert.ok(at >= 0, 'GateToNote names a channel');

  // Left alone it is a voice, not a kit: channel 1, nothing to do with drums.
  assert.equal(drumSources(device, patch).length, 0);

  node.params[at] = 10;
  const [source] = drumSources(device, patch);
  assert.ok(source, 'a Euclidean pattern through a GateToNote on channel 10 is drums');
  assert.equal(source.key, 'node:0');
  assert.equal(source.kind, 'note');
  assert.deepEqual(source.buses, node.outBuses[0], 'heard on the bus it writes');
  assert.equal(source.channel, 10, 'and only the notes on that channel are its own');

  // A drum sequencer is drums because of what it is rather than where it
  // sends, so it claims its bus whatever channel its lanes are on.
  patched(device, patch, idOf('DrumSeqMidi', device));
  const sources = drumSources(device, patch);
  assert.equal(sources.length, 2);
  assert.equal(sources[1].channel, null, 'a drum sequencer takes the whole bus');

  // An outlet on no bus is nothing to listen to, and a node with no note
  // outlet at all cannot be one however its channel is set.
  node.outBuses[0] = [];
  assert.equal(drumSources(device, patch).length, 1);
});

test('a bus that is drums by its channel is drums only on that channel', async () => {
  const { listener, module } = await listening();
  listener.setDrumSources([
    { key: 'node:0', index: 0, label: 'GateToNote 0', kind: 'note', channel: 10, buses: [3], lanes: [] },
  ]);
  assert.equal(module.watches.get(3), 1, 'its bus is read without a player on it');

  // Nobody is listening to bus 3 and it is heard anyway - which is the whole
  // point: an instrument is audible because it is in the patch.
  listener.noteBus({ t: 0, bus: 3, type: 0x90, channel: 10, d1: 36, d2: 100 });
  assert.equal(listener.drums.get('node:0').hits, 1);
  assert.equal(listener.players[0].voices.size, 0, 'no player may sound a drum note');
  assert.equal(listener.drums.get('other').hits, 0, 'and the catch-all does not play it as well');

  // A bus takes as many writers as somebody patches to it, and a melody
  // sharing one with a drum is still a melody.
  const player = listener.addPlayer({ source: 'bus', bus: 3 });
  listener.noteBus({ t: 0, bus: 3, type: 0x90, channel: 1, d1: 60, d2: 100 });
  assert.equal(listener.drums.get('node:0').hits, 1, 'a melody on the same bus is not a drum');
  assert.equal(player.voices.size, 1, 'it goes to the player listening to that bus');

  // The other kind: a drum sequencer's bus is drums on every channel.
  listener.setDrumSources([
    { key: 'node:1', index: 1, label: 'DrumSeqMidi 1', kind: 'note', channel: null, buses: [5], lanes: [] },
  ]);
  listener.noteBus({ t: 0, bus: 5, type: 0x90, channel: 1, d1: 38, d2: 100 });
  assert.equal(listener.drums.get('node:1').hits, 1);
});

test('a hit already heard on its bus is not heard again off the cable', async () => {
  const { listener } = await listening();
  listener.setDrumSources([
    { key: 'node:0', index: 0, label: 'DrumSeqMidi 0', kind: 'note', buses: [3], lanes: [] },
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

test('a gate lane plays its drum on the edge, not while the gate is high', async () => {
  const { listener, module } = await listening();
  listener.setDrumSources([
    { key: 'node:1', index: 1, label: 'DrumSeqGate 1', kind: 'gate', buses: [],
      lanes: [{ lane: 0, buses: [4], piece: 'kick' }, { lane: 1, buses: [5], piece: 'snare' }] },
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

test('the gate listener hears what it was pointed at, and each edge once', async () => {
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

// A note sounds where its own time falls on the audio clock. The pairing of
// the clocks is between the wall clock and the audio clock, not between the
// module's time and the audio clock: a rebuild of the page runs the module
// ahead of the wall, and a note it computed early must still sound on time.
test('a note sounds at its own time, wherever the module had got to', async () => {
  const { listener, module, ctx } = await listening();
  // The fake module's wall clock is `performance.now()`, so a note a second
  // from now is a second from now.
  const soon = Math.round(performance.now()) * 1000 + 1_000_000;
  listener.anchor();
  const first = listener.when(soon);
  // The module is now half a second ahead of the wall clock, and the frame
  // pairs the clocks again: a note a tenth of a second after the first one
  // is scheduled a tenth of a second after it.
  module.now = soon + 500_000;
  listener.anchor();
  const second = listener.when(soon + 100_000);
  assert.ok(Math.abs(second - first - 0.1) < 0.005, `the notes are ${second - first} s apart`);
  // What a stall made late sounds now rather than never.
  ctx.currentTime = 1e9;
  assert.ok(listener.when(0) >= 1e9 + 0.002, 'a note in the past was scheduled there');
  // And a panic ends what is sounding at the module's time - after every
  // note it has scheduled so far, never before one.
  ctx.currentTime = 0;
  const player = listener.players[0];
  listener.midi({ t: soon + 500_000, type: 0x90, d1: 60, d2: 100, channel: 1 });
  const voice = [...player.voices.values()][0];
  assert.ok(voice, 'the note was not played');
  listener.panic();
  assert.ok(voice.osc.stoppedAt > voice.osc.startedAt, 'the panic stopped the note before it started');
});

test('what is being listened to survives a reload', async () => {
  const { listener } = await listening();
  listener.setDrumSources([
    { key: 'node:0', index: 0, label: 'DrumSeqMidi 0', kind: 'note', buses: [3], lanes: [] },
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

// --- the macro bench ---------------------------------------------------------

// A destination carries a window, a signed depth and a target. Those are
// numbers rather than gestures, which is why they are here and not on the
// performance surface - and why the panel has to draw them: four rows of
// numbers do not say what a macro does.
test('the macro bench draws a window per destination, and offers the parameters', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const divider = device.algorithms.find((a) => a.name === 'ClockDiv');
  patch.nodes = [codec.emptyNode(divider.id)];
  patch.macros[0] = { name: 'open up' };
  // Two windows on one parameter, pushing opposite ways: the rise-and-fall a
  // single destination deliberately cannot do, because holding is the
  // primitive (src/node/patch.h).
  const dest = (srcLo, srcHi, depth) => ({
    macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0,
    param: 1, srcLo, srcHi, depth, flags: 0,
  });
  patch.macroDest[0] = dest(0, 128, 90);
  patch.macroDest[1] = dest(128, 255, -60);

  const app = fakeApp({ patch, device });
  const panel = withDom(() => Macros(app));
  const lanes = findAll(panel, (n) => String(n.className).includes('lane-line'));
  assert.equal(lanes.length, 2, 'one lane drawn per destination');

  // The target pickers are the mod matrix's, and the parameter list is the
  // module's own descriptors: a picker with nothing in it would be a
  // destination nobody could point anywhere.
  const params = find(panel, (n) => n.attrs['aria-label'] === 'which parameter');
  assert.ok(params.children.length > 1, 'the parameters of the node are offered');

  // A macro may not reach a macro - the firmware refuses it - so it is not
  // offered rather than refused after the fact.
  const kinds = find(panel, (n) => n.attrs['aria-label'] === 'what kind of target');
  assert.ok(!words(kinds).includes('a macro'), 'a destination cannot target a macro');

  // The name field stops at the width the patch holds.
  const name = find(panel, (n) => n.attrs['aria-label'] === 'the name of macro 1');
  assert.equal(name.getAttribute('maxlength'), String(device.capabilities.macroNameBytes));
});

// Silent, clipped and working look identical in a table of numbers and are
// three different problems. The module reports all three; the panel draws
// them apart.
test('the bench tells a silent destination from a clipped one', async () => {
  const { module } = await instantiate();
  const device = await connected(module);
  const patch = codec.emptyPatch();
  const divider = device.algorithms.find((a) => a.name === 'ClockDiv');
  patch.nodes = [codec.emptyNode(divider.id)];
  patch.macros[0] = { name: 'lift' };
  patch.macroDest[0] = {
    macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0,
    param: 1, srcLo: 0, srcHi: 255, depth: 90, flags: 0,
  };
  patch.macroDest[1] = {
    macro: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0,
    param: 2, srcLo: 0, srcHi: 255, depth: -20, flags: 0,
  };

  const live = new Map([[0, {
    macro: 0, engaged: true, position: 200,
    dests: [
      { slot: 0, status: P.ModStatus.MOD_STATUS_CLIPPED, offset: 90, anchor: 100, value: 255 },
      { slot: 1, status: P.ModStatus.MOD_STATUS_SILENT, offset: 0, anchor: 100, value: 100 },
    ],
  }]]);
  const app = fakeApp({ patch, device, macroLive: live });
  const panel = withDom(() => Macros(app));
  assert.ok(find(panel, (n) => String(n.className).includes('is-clipped')), 'the pinned one says so');
  assert.ok(find(panel, (n) => String(n.className).includes('is-silent')), 'the dark one is not the same as broken');
  assert.ok(words(panel).includes('pinned against the end of its range'));
  assert.ok(words(panel).includes('silent until the macro is moved'));

  // The pool budget is read from the module rather than written into the
  // panel, so cutting the pool is a firmware change and not two.
  assert.ok(words(panel).includes(`${device.capabilities.macroDests} destinations in the pool`));
});


// --- the performance surface ---------------------------------------------------

// The surface over a module that records what it was handed: the cable and the
// channel a control claims are only real if they arrive at the module that
// way, so the test reads the module's own argument list rather than a spy on
// the service above it.
function surfaced({ patch = codec.emptyPatch() } = {}) {
  const heard = [];
  const module = {
    deliverMidi(port, type, channel, d1, d2) { heard.push({ port, type, channel, d1, d2 }); return 1; },
  };
  const app = fakeApp({ patch, module });
  const play = new Play({
    state: app.state, session: app.session, module: () => module, render: () => {},
  });
  const surface = new SurfaceDoc({
    state: app.state, library: { readSurface: () => null, writeSurface: () => {} },
    play, editor: app.editor, session: app.session, render: () => {},
  });
  Object.assign(app, { play, surface, heard });
  return app;
}

const NOTE_ON = 0x90;
const NOTE_OFF = 0x80;

test('the surface is on one lead, and the keyboard on its own', async () => {
  const app = surfaced();
  // One lead for all twenty-four controls, the way a controller has one: the
  // cable decides what in the patch can hear the thing at all, and a box of
  // pads each on a different lead is not a controller anybody could reason
  // about. The channel stays the control's own.
  app.surface.setPort(P.MidiPort.mmMIDI_SERIAL_1);
  app.surface.setPad(0, { channel: 5 });
  app.surface.press(0);
  app.surface.release(0);
  app.surface.setPot(0, { channel: 9 });
  app.surface.turn(0, 64, { commit: true });
  // The keyboard summoned over the surface is a separate instrument: playing
  // a part into one input while the pads drive another is the ordinary case.
  app.play.noteOn(60, 100);

  assert.deepEqual(app.heard.map((e) => [e.type, e.port, e.channel, e.d1]), [
    [NOTE_ON, P.MidiPort.mmMIDI_SERIAL_1, 5, 36],
    [NOTE_OFF, P.MidiPort.mmMIDI_SERIAL_1, 5, 36],
    [0xb0, P.MidiPort.mmMIDI_SERIAL_1, 9, 20],
    [NOTE_ON, app.state.ui.play.port, app.state.ui.play.channel, 60],
  ]);
  assert.notEqual(app.state.ui.play.port, P.MidiPort.mmMIDI_SERIAL_1,
                  'the keyboard did not follow the surface onto its lead');
});

test('a binding learned on another cable is not this control’s', async () => {
  const app = surfaced();
  const pot = app.surface.pot(0);
  app.state.patch.ccMap[0] = {
    sourceMask: P.MidiPort.mmMIDI_USB_1, channel: 0, cc: pot.cc,
    targetKind: P.CcTargetKind.CC_TARGET_MACRO, targetIndex: 0, param: 0,
    min: 0, max: 0, flags: 0,
  };
  // The module's own learn writes the single port the CC arrived on
  // (src/control/cc_mapper.cpp), so moving the surface's lead leaves every
  // binding learned on the old one behind - and the sheet must not go on
  // saying a control still reaches what it no longer does.
  assert.equal(app.surface.bindingOf(pot), null, 'the cable is part of the match');
  app.surface.setPort(P.MidiPort.mmMIDI_USB_1);
  assert.equal(app.surface.bindingOf(pot)?.slot, 0);
});

test('a pad held is a note held, and edit is a mode', async () => {
  const app = surfaced();
  const pads = (view) => findAll(view, (n) => String(n.className).split(/\s+/).includes('pad'));

  // Playing: press, hold as long as you like, release. Nothing in the gesture
  // asks what the pad does, which is the whole point - a long press used to
  // open the sheet and take the note with it.
  const playing = withDom(() => SurfaceView(app));
  const button = pads(playing)[0];
  button.fire('pointerdown', { pointerId: 1, clientX: 0, clientY: 0, preventDefault: () => {} });
  button.fire('pointerup', { pointerId: 1, preventDefault: () => {} });
  assert.deepEqual(app.heard.map((e) => e.type), [NOTE_ON, NOTE_OFF]);
  assert.equal(app.state.ui.surface.editing, null, 'playing a pad does not open its sheet');

  // Editing: the same press opens the sheet and sends nothing.
  app.state.ui.surface.edit = true;
  const editing = withDom(() => SurfaceView(app));
  assert.ok(String(editing.className).includes('editing'), 'the mode is on the surface itself');
  const second = pads(editing)[1];
  second.fire('pointerdown', { pointerId: 2, clientX: 0, clientY: 0, preventDefault: () => {} });
  assert.deepEqual(app.state.ui.surface.editing, { kind: 'pad', index: 1 });
  assert.equal(app.heard.length, 2, 'a pad in edit mode plays nothing');
});

test('a finger held on a pad is a long note, not a question', async () => {
  const app = surfaced();
  const view = withDom(() => SurfaceView(app));
  const pad = findAll(view, (n) => String(n.className).split(/\s+/).includes('pad'))[0];
  const touch = { pointerId: 1, pointerType: 'touch', clientX: 0, clientY: 0, preventDefault: () => {} };

  // A touch browser reports its own long press as `contextmenu` - the same
  // event a mouse's right button sends - so a note held past half a second
  // arrived at the sheet through the one door left open.
  pad.fire('pointerdown', touch);
  pad.fire('contextmenu', { preventDefault: () => {} });
  assert.equal(app.state.ui.surface.editing, null, 'a held finger does not open the sheet');
  assert.deepEqual(app.heard.map((e) => e.type), [NOTE_ON], 'and the note is still sounding');
  pad.fire('pointerup', touch);
  assert.deepEqual(app.heard.map((e) => e.type), [NOTE_ON, NOTE_OFF]);

  // A right button on a mouse is still asking it: that is not a long press,
  // and it is how a control is set up without leaving the mode on.
  pad.fire('pointerdown', { pointerId: 2, pointerType: 'mouse', clientX: 0, clientY: 0, preventDefault: () => {} });
  pad.fire('contextmenu', { preventDefault: () => {} });
  assert.deepEqual(app.state.ui.surface.editing, { kind: 'pad', index: 0 });
});

test('the lead is chosen once, in the bar, and the sheet says which it is', async () => {
  const app = surfaced();
  app.surface.setPort(P.MidiPort.mmMIDI_SERIAL_2);

  // A per-control cable would be twenty-four answers to a question a
  // controller asks once, so the sheet has the channel and no port picker -
  // and says the lead, because a channel means nothing without it.
  const sheet = withDom(() => AssignSheet(app, { kind: 'pad', index: 0 }));
  assert.ok(find(sheet, (n) => n.attrs['aria-label'] === 'channel'));
  assert.equal(find(sheet, (n) => n.attrs['aria-label'] === 'the module input this surface plays into'),
               null, 'the cable is not a per-control field');
  assert.ok(words(sheet).includes('the whole surface sends on DIN 2'));

  // The bar is where it is chosen, and only while the surface is being set
  // up: playing, that corner says which machine the lead reaches.
  app.state.ui.surface.edit = true;
  const bar = withDom(() => SurfaceView(app));
  const picker = find(bar, (n) => n.attrs['aria-label'] === 'the module input this surface plays into');
  assert.ok(picker, 'the surface names its own cable in the bar');
});

test('a number in the sheet is typed as well as nudged', async () => {
  const app = surfaced();
  app.surface.setPad(0, { kind: PadKind.CC });
  const field = (label) => find(withDom(() => AssignSheet(app, { kind: 'pad', index: 0 })),
                                (n) => n.tag === 'input' && n.attrs['aria-label'] === label);

  // Walking a CC from 40 to 102 one press at a time is not editing, it is
  // waiting: the value between the two targets is a field.
  field('CC number').fire('change', { target: { value: '102' } });
  assert.equal(app.surface.pad(0).cc, 102);
  // What is typed is clamped rather than sent: the number is a CC.
  field('CC number').fire('change', { target: { value: '400' } });
  assert.equal(app.surface.pad(0).cc, 119);

  // And the thumb targets are still either side of it, because the sheet is
  // read over a surface being played with one hand.
  const sheet = withDom(() => AssignSheet(app, { kind: 'pad', index: 0 }));
  find(sheet, (n) => n.attrs['aria-label'] === 'CC number down').fire('click');
  assert.equal(app.surface.pad(0).cc, 118);
});

test('a CC pad sends the two values it was given', async () => {
  const app = surfaced();
  // 127 and 0 is the commonest pair, not the only one: a latch alternating
  // between two points of a parameter is the same pad with two other numbers
  // in it.
  app.surface.setPad(0, { kind: PadKind.CC, mode: PadMode.TOGGLE, ccOn: 64, ccOff: 20 });
  assert.equal(app.surface.press(0), '64', 'the display says the number that went out');
  assert.equal(app.surface.press(0), '20');
  app.surface.setPad(1, { kind: PadKind.CC, ccOn: 100, ccOff: 5 });
  app.surface.press(1);
  app.surface.release(1);
  assert.deepEqual(app.heard.map((e) => [e.d1, e.d2]),
                   [[40, 64], [40, 20], [41, 100], [41, 5]],
                   'a momentary pad is the same pair, one on the way down and one on the way up');

  // Named for what the pad does: a latch is on and off, a momentary pad is
  // pressed and released.
  const latch = withDom(() => AssignSheet(app, { kind: 'pad', index: 0 }));
  assert.ok(find(latch, (n) => n.attrs['aria-label'] === 'the value it sends on'));
  assert.ok(words(latch).includes('on') && words(latch).includes('off'));
  const held = withDom(() => AssignSheet(app, { kind: 'pad', index: 1 }));
  assert.ok(words(held).includes('pressed') && words(held).includes('released'));
});

test('a pad on a macro is silkscreened with the macro’s name', async () => {
  const app = surfaced();
  app.state.patch.macros[0] = { name: 'sweep' };
  app.surface.setPad(0, { kind: PadKind.CC });
  const pad = app.surface.pad(0);
  app.state.patch.ccMap[0] = {
    sourceMask: app.surface.port, channel: 0, cc: pad.cc,
    targetKind: P.CcTargetKind.CC_TARGET_MACRO, targetIndex: 0, param: 0,
    min: 0, max: 0, flags: 0,
  };

  // A pad put on a macro *is* that macro to whoever is playing it, exactly as
  // a pot on one is: reading "cc 40" off sixteen pads is reading a wiring
  // diagram. What it sends stays on the line underneath.
  const first = findAll(withDom(() => SurfaceView(app)),
                        (n) => String(n.className).split(/\s+/).includes('pad'))[0];
  assert.ok(words(first).includes('sweep'), 'the pad names the macro it drives');
  assert.ok(words(first).includes(`cc ${pad.cc}`), 'and still says what it sends');

  // A note pad has no binding to read whatever number is sitting in its `cc`:
  // a note goes into the patch, not through the binding table.
  app.surface.setPad(0, { kind: PadKind.NOTE });
  const note = findAll(withDom(() => SurfaceView(app)),
                       (n) => String(n.className).split(/\s+/).includes('pad'))[0];
  assert.ok(!words(note).includes('sweep'));
});

// --- the keyboard --------------------------------------------------------------

test('a white key is notched where a black key stands over it', async () => {
  const keyboard = withDom(() => Keyboard({
    from: 60, to: 72, onNoteOn: () => {}, onNoteOff: () => {},
  }));
  const key = (name) => find(keyboard, (n) => n.attrs['aria-label'] === name);
  const cuts = (name) => String(key(name).className).split(/\s+/).filter((c) => c.startsWith('pk-cut'));

  // A rectangle under the black keys lights a slab of colour when it is held,
  // which reads as a rendering fault rather than a note. D has a black key
  // either side of its top; E has one only below it.
  assert.deepEqual(cuts('D4'), ['pk-cut-l', 'pk-cut-r']);
  assert.deepEqual(cuts('E4'), ['pk-cut-l']);
  assert.deepEqual(cuts('F4'), ['pk-cut-r']);
  // C4 is the bottom of this range: there is no B3 above it to hide behind,
  // so its top is whole on that side.
  assert.deepEqual(cuts('C4'), ['pk-cut-r']);
  assert.deepEqual(cuts('C5'), [], 'and the last key of the range keeps both shoulders');
});
