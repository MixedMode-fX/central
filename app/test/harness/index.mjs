// What the app's tests stand the app up on: the real module, and fakes for
// the three things Node has not got - a document, Web Audio, and a browser's
// localStorage. Every fake is deliberately small, and honest about it: the
// sequences fired at them come from a real browser.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';

import * as P from '../../src/protocol/generated.js';
import { Device } from '../../src/protocol/device.js';
import { EmbeddedModule } from '../../src/runtime/module.js';
import { Listener } from '../../src/runtime/audio/listener.js';
import { createState } from '../../src/services/state.js';
import { Live } from '../../src/services/render.js';
import { emptyNode } from '../../src/protocol/codec.js';

const here = dirname(fileURLToPath(import.meta.url));
export const wasmPath = process.env.MMMC_WASM ?? join(here, '..', '..', '..', 'emulator', 'dist', 'mmmc.wasm');
export const repoRoot = join(here, '..', '..', '..');

// --- the module -------------------------------------------------------------

export async function instantiate() {
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
export async function connected(module) {
  const device = new Device(module);
  await device.readCapabilities();
  await device.readAlgorithms();
  for (const descriptor of device.algorithms) await device.readParams(descriptor.id);
  return device;
}

// --- the app, as a view sees it ---------------------------------------------

// Enough of the app for a panel to be built from: the state, the device, and
// an editor whose every command is recorded rather than sent. `calls` is what
// a test reads back to see what a press asked for.
export function fakeApp({ patch, globals, device = null, module = null, controller = null,
                          offline = false, modLive = new Map(), macroLive = new Map() } = {}) {
  const state = createState();
  if (patch) state.patch = patch;
  if (globals) state.globals = globals;
  const calls = [];
  const record = (name) => (...args) => { calls.push([name, ...args]); };
  const editor = {
    learnTarget: null,
    isArmed(index, param) {
      return Boolean(this.learnTarget && this.learnTarget.nodeIndex === index && this.learnTarget.param === param);
    },
    learn(index, param) { calls.push(['learn', index, param]); this.learnTarget = { nodeIndex: index, param }; },
    cancelLearn() { calls.push(['cancel']); this.learnTarget = null; },
    setGlobals(changes, what = 'settings') { Object.assign(state.globals, changes); calls.push(['globals', changes, what]); },
  };
  for (const name of ['bindParam', 'clearMapping', 'routeParam', 'clearModRoute', 'setParam', 'setParams',
                      'setConnection', 'setJack', 'setMidiPort', 'setModRoute', 'setCcMap', 'learnInto',
                      'removeNode', 'removeBlock', 'applyPlan', 'add', 'addMidiPort',
                      'setMacro', 'setMacroDest', 'clearMacroDest', 'addMacroDest']) {
    editor[name] = record(name);
  }
  editor.caps = device?.capabilities ?? null;
  return {
    state, device, module, controller, calls, editor,
    live: new Live(),
    session: { offline, usingModule: Boolean(module), modLive, macroLive },
    render: () => {},
    say: record('say'),
    fail: record('fail'),
  };
}

// --- localStorage ----------------------------------------------------------

// Strings in, strings out, and a quota that can refuse.
export function fakeStorage({ limit = Infinity } = {}) {
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

// --- a document ---------------------------------------------------------------

// Enough of a document for `el()` to build an element and for a test to fire
// events at it. The selector shapes the views use, and no more: a tag, a
// class, and an attribute by presence or by value.
export function fakeDocument() {
  const matches = (node, selector) => {
    if (node.nodeType !== 1) return false;
    if (selector.startsWith('.')) return String(node.className).split(/\s+/).includes(selector.slice(1));
    const attr = /^\[([\w-]+)(?:="([^"]*)")?\]$/.exec(selector);
    if (attr) return attr[2] === undefined ? attr[1] in node.attrs : node.attrs[attr[1]] === attr[2];
    return node.tag === selector;
  };
  const descendants = (node, out = []) => {
    for (const kid of node.children ?? []) { out.push(kid); descendants(kid, out); }
    return out;
  };
  const adopt = (parent, kids) => {
    for (const kid of kids) if (kid && kid.nodeType === 1) kid.parent = parent;
  };
  const make = (tag) => {
    const listeners = new Map();
    const element = {
      tag, nodeType: 1, className: '', attrs: {}, value: '', children: [], parent: null,
      style: {}, textContent: '', isConnected: true,
      setAttribute(key, value) {
        this.attrs[key] = String(value);
        if (key === 'value') this.value = String(value);
        if (key === 'class') this.className = String(value);
      },
      getAttribute(key) { return this.attrs[key] ?? null; },
      addEventListener(type, fn) {
        if (!listeners.has(type)) listeners.set(type, []);
        listeners.get(type).push(fn);
      },
      // A child knows its parent, so it can take itself off the page the
      // way a menu does when the next one opens.
      append(...kids) { adopt(this, kids); this.children.push(...kids); },
      replaceChildren(...kids) { adopt(this, kids); this.children = [...kids]; },
      fire(type, event = {}) { for (const fn of listeners.get(type) ?? []) fn({ type, ...event }); },
      getBoundingClientRect: () => ({ left: 0, top: 0, bottom: 0, right: 0 }),
      querySelector(selector) { return descendants(this).find((kid) => matches(kid, selector)) ?? null; },
      querySelectorAll(selector) { return descendants(this).filter((kid) => matches(kid, selector)); },
      contains: () => false,
      focus() {},
      remove() {
        const from = this.parent ?? document.body;
        const where = from.children.indexOf(this);
        if (where >= 0) from.children.splice(where, 1);
        this.parent = null;
      },
      get firstChild() { return this.children[0] ?? null; },
    };
    Object.defineProperty(element, 'dataset', {
      get() {
        const out = {};
        for (const [key, value] of Object.entries(this.attrs)) {
          if (!key.startsWith('data-')) continue;
          out[key.slice(5).replace(/-(.)/g, (_, c) => c.toUpperCase())] = value;
        }
        return out;
      },
    });
    element.classList = {
      add: (name) => { if (!matches(element, `.${name}`)) element.className = `${element.className} ${name}`.trim(); },
      remove: (name) => {
        element.className = String(element.className).split(/\s+/).filter((c) => c && c !== name).join(' ');
      },
      contains: (name) => matches(element, `.${name}`),
      toggle: (name, on) => (on ? element.classList.add(name) : element.classList.remove(name)),
    };
    return element;
  };
  const body = make('body');
  const document = {
    createTextNode(text) { return { nodeType: 3, text: String(text) }; },
    createElement: make,
    createElementNS: (_ns, tag) => make(tag),
    body,
    getElementById(id) { return descendants(body).find((kid) => kid.attrs?.id === id) ?? null; },
    querySelector(selector) { return body.querySelector(selector); },
    querySelectorAll(selector) { return body.querySelectorAll(selector); },
  };
  return document;
}

// Everything a panel wrote, as one string: what a user would read off it.
export function words(node) {
  return [node?.text ?? '', node?.textContent ?? '',
          ...(node?.children ?? []).map(words)].flat().join(' ');
}

// The first element in a panel that answers a question - "is there a select
// in here" - without the test having to know how deep it was nested.
export function find(node, matches) {
  for (const kid of node?.children ?? []) {
    if (kid.nodeType === 1 && matches(kid)) return kid;
    const deeper = find(kid, matches);
    if (deeper) return deeper;
  }
  return null;
}

// Every element that answers, for a question about how many of something a
// panel drew - one lane per destination, one row per slot.
export function findAll(node, matches, out = []) {
  for (const kid of node?.children ?? []) {
    if (kid.nodeType === 1 && matches(kid)) out.push(kid);
    findAll(kid, matches, out);
  }
  return out;
}

// The views read `document` when they build something, not when they load,
// so the fake only has to stand up for the call itself.
export function withDom(fn) {
  const had = globalThis.document;
  globalThis.document = fakeDocument();
  const restore = () => { globalThis.document = had; };
  try {
    const result = fn();
    return result instanceof Promise ? result.finally(restore) : (restore(), result);
  } catch (error) {
    restore();
    throw error;
  }
}

// --- Web Audio -------------------------------------------------------------

// Every node records when it was started and stopped, because "a kit that
// throws on the crash" and "a hit scheduled in the past" are the two ways a
// recipe goes wrong, and neither makes a sound to notice.
export function fakeAudioContext() {
  const made = [];
  const param = () => ({
    value: 0,
    setValueAtTime() { return this; },
    linearRampToValueAtTime() { return this; },
    exponentialRampToValueAtTime() { return this; },
    setTargetAtTime() { return this; },
  });
  const node = (kind, extra = {}) => {
    const built = {
      kind, startedAt: null, stoppedAt: null,
      connect(to) { return to; },
      disconnect() {},
      start(at = 0) { this.startedAt = at; },
      stop(at = 0) { this.stoppedAt = at; },
      ...extra,
    };
    made.push(built);
    return built;
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
// levels of the current pass, and the note-bus watches.
export function fakeModule() {
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
// it is where the master gain, the click gain and every voice get wired up.
export async function listening() {
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

// --- a patch to test something else with ------------------------------------

// A node appended and patched in, for the tests whose subject is something
// else and which need a patch that runs: required inlets onto the bus the last
// node of that domain wrote, the first outlet onto a bus nothing writes yet.
//
// **The editor does none of this.** A node it adds arrives unconnected and the
// wires are the ones you drag (src/core/graph.js), so this is the tests' own
// shorthand for a chain rather than a second copy of a rule the app has. What
// the editor really does on "add" is `protocol.test.mjs`'s to check.
export function patched(device, patch, descriptor) {
  const d = typeof descriptor === 'number' ? device.byId.get(descriptor) : descriptor;
  const node = emptyNode(d.id);
  const written = new Set();
  const outletsOf = (n, dd) => {
    for (let i = 0; i < dd.nOut && i < P.MAX_OUT; i++) {
      if (n.outBus[i] !== P.NO_BUS) written.add(`${dd.outDomain[i]}:${n.outBus[i]}`);
    }
  };
  patch.nodes.forEach((other) => {
    const od = device.byId.get(other.algorithmId);
    if (od) outletsOf(other, od);
  });

  for (let i = 0; i < d.minIn && i < d.nIn && i < P.MAX_IN; i++) {
    const domain = d.inDomain[i];
    let bus = 0;
    for (let n = patch.nodes.length - 1; n >= 0 && bus === 0; n--) {
      const od = device.byId.get(patch.nodes[n].algorithmId);
      for (let k = 0; od && k < od.nOut && k < P.MAX_OUT; k++) {
        if (od.outDomain[k] === domain && patch.nodes[n].outBus[k] !== P.NO_BUS) {
          bus = patch.nodes[n].outBus[k];
          break;
        }
      }
    }
    node.inBus[i] = bus;
  }
  if (d.nOut > 0) {
    const domain = d.outDomain[0];
    const buses = busCountOf(device, domain);
    for (let b = 0; b < buses; b++) {
      if (written.has(`${domain}:${b}`)) continue;
      node.outBus[0] = b;
      break;
    }
  }
  patch.nodes.push(node);
  return node;
}

const busCountOf = (device, domain) => [
  device.capabilities.gateBuses, device.capabilities.noteBuses, device.capabilities.cvBuses,
][domain];
