// A patch, in words.
//
// `.syx` is the patch *image*: the bytes the module stores, which is what a
// librarian sends and what a preset slot holds. It is exactly the right thing
// for hardware and no use at all to a person - it cannot be read, diffed,
// pasted into an issue or written by hand.
//
// This is the same patch as JSON: algorithms by name, jacks numbered from 1 as
// on the panel, MIDI ports as a user knows them, bus indices per domain as in
// `NodeConfig`. It is the dialect the emulator page took before the editor and
// the emulator became one app, so patches written for that page still load,
// and it is how the example patches in `examples.js` are written.
//
// It carries `globals` and `cc_map` too. An export that dropped the tempo and
// the controller bindings would be a lossy copy of the patch it claims to be.
//
// A sequencer node may carry a `seq` block instead of raw `params`, which is
// packed into the parameter layout the firmware documents in
// `note_sequencer.h`, `drum_sequencer.h` and `gate_sequencer.h`. Nobody writes
// a drum pattern as a list of 336 bytes. `Metronome` takes one too - it is a
// clock node rather than a sequencer, but "division: 1/8, feel: triplet" is
// the same idea: the controls in words rather than in bytes.

import * as P from './protocol.js';
import * as codec from './codec.js';
import { MIDI_PORTS, portNames, scaleMaskOf, scaleIdOf, scaleName, STEP_DIRECTIONS,
         METRONOME_DIVISIONS, METRONOME_FEELS } from './names.js';
import { modParamName } from './graph.js';

const DIRECTIONS = { [P.GatePortDirection.GATE_PORT_UNUSED]: 'unused',
                     [P.GatePortDirection.GATE_PORT_IN]: 'in',
                     [P.GatePortDirection.GATE_PORT_OUT]: 'out' };

const busOrNull = (bus) => (bus === P.NO_BUS || bus === undefined ? null : bus);

// Trailing zeros are not carried: a zero parameter means the descriptor's
// default, and the emulator only writes the non-zero ones. Same rule the patch
// image uses, so the JSON is the same size story as the .syx.
function usedParams(params) {
  let n = params.length;
  while (n > 0 && !params[n - 1]) n--;
  return Array.from(params.subarray ? params.subarray(0, n) : params.slice(0, n));
}

// An inlet or outlet array only needs to reach the last connected one - the
// emulator leaves the rest at NO_BUS.
function usedBuses(buses, count) {
  const out = [];
  for (let i = 0; i < count; i++) out.push(busOrNull(buses[i]));
  while (out.length && out[out.length - 1] === null) out.pop();
  return out;
}

// --- the sequencer sugar ---------------------------------------------------
//
// The offsets below are the firmware's parameter layout, not a format of this
// file's own: SEQ_HEADER and SEQ_LANE_STRIDE are note_sequencer.h and
// drum_sequencer.h, and the flag bits are NoteStep's.
const SEQ_HEADER = 16, SEQ_LANE_STRIDE = 8;
const NOTE_FLAG = { rest: 0x20, tie: 0x40, accent: 0x80 };
const GATE_SEQUENCERS = ['StepSequencer', 'EuclidianSequencer', 'RandomSequencer'];

// A sequencer's scale, as the 12-bit mask the firmware stores. Omitted - or
// named "global" - is an empty mask, and an empty mask is the firmware's
// "follow the module's key" (src/midi/global_scale.h).
const scaleOf = (v) => (v === undefined ? 0 : (typeof v === 'number' ? v & 0xfff : scaleMaskOf(v)));

function directionOf(v) {
  if (v === undefined) return 0;
  if (typeof v === 'number') return v;
  const at = STEP_DIRECTIONS.indexOf(String(v).toLowerCase().replace(/[\s_-]+/g, ''));
  if (at < 0) throw new Error(`unknown direction "${v}" (one of ${STEP_DIRECTIONS.join(', ')})`);
  return at;
}

// A Metronome's note value and its feel, by the names on the control rather
// than by the byte behind it. Both enums count from 1 (metronome.h), so a
// name is its index plus one, and `undefined` is left as 0 - the parameter's
// own default - rather than guessed at here.
function fromList(list, v, what) {
  if (v === undefined) return 0;
  if (typeof v === 'number') return v;
  const key = (x) => String(x).toLowerCase().replace(/[\s_-]+/g, '');
  const at = list.map(key).indexOf(key(v));
  if (at < 0) throw new Error(`unknown ${what} "${v}" (one of ${list.join(', ')})`);
  return at + 1;
}

// A hit is a velocity: 'x' is 100, 'X' 127, 'o' 60, '1'-'9' a ninth of the
// range each, and anything else is off. So a lane reads as a pattern.
function hitVelocity(ch) {
  if (ch === 'x') return 100;
  if (ch === 'X') return 127;
  if (ch === 'o') return 60;
  if (ch >= '1' && ch <= '9') return Number(ch) * 14;
  return 0;
}

function hitBits(hits) {
  let bits = 0;
  [...String(hits)].forEach((ch, i) => { if (i < P.MAX_SEQUENCE_LEN && hitVelocity(ch)) bits |= 1 << i; });
  return bits >>> 0;
}

function noteStep(step, at) {
  if (typeof step === 'number') return { deg: step };
  if (typeof step !== 'string') return step;
  if (step === '-' || step === 'rest' || step === '.') return { rest: true };
  if (step === '=' || step === 'tie') return { tie: true };
  throw new Error(`step ${at}: "${step}"? use a degree, "-" (a rest), "=" (a tie) or an object`);
}

export function packSeq(name, seq, params) {
  const u8 = (i, v) => {
    if (i >= params.length) throw new Error(`${name}: parameter ${i} is beyond N_PARAM`);
    params[i] = v & 0xff;
  };
  const header = () => {
    if (seq.length !== undefined) u8(0, seq.length);
    u8(1, directionOf(seq.direction));
  };

  if (name === 'NoteSequencer' || name === 'PolySequencer') {
    const voices = name === 'PolySequencer' ? P.NOTE_SEQ_VOICES : 1;
    const stride = voices * 2 + 2;
    header();
    u8(2, seq.gate ?? 0);
    const mask = scaleOf(seq.scale);
    u8(3, mask & 0xff); u8(4, mask >> 8);
    u8(5, seq.root ?? 0); u8(6, seq.velScale ?? 0); u8(7, seq.velOffset ?? 0);
    u8(8, seq.channel ?? 0); u8(9, seq.accent ?? 0); u8(10, seq.stall ?? 0);
    const steps = seq.steps ?? [];
    if (steps.length > P.MAX_SEQUENCE_LEN) throw new Error(`${name}: at most ${P.MAX_SEQUENCE_LEN} steps`);
    if (seq.length === undefined) u8(0, steps.length);
    steps.forEach((step, i) => {
      const o = noteStep(step, i);
      const degrees = Array.isArray(o.deg) ? o.deg : [o.deg ?? 0];
      const velocities = Array.isArray(o.vel) ? o.vel : [o.vel ?? 100];
      const base = SEQ_HEADER + i * stride;
      for (let v = 0; v < voices; v++) {
        const sounds = v < degrees.length || (v === 0 && !Array.isArray(o.deg));
        u8(base + v * 2, sounds ? degrees[v] : 0);
        u8(base + v * 2 + 1, sounds ? (velocities[v] ?? velocities[0] ?? 100) : 0);
      }
      const flags = (o.rest ? NOTE_FLAG.rest : 0) | (o.tie ? NOTE_FLAG.tie : 0)
                  | (o.accent ? NOTE_FLAG.accent : 0);
      u8(base + voices * 2, ((o.len ?? 1) & 0x1f) | flags);
      u8(base + voices * 2 + 1, o.prob ?? 0);
    });
    return;
  }

  if (name === 'DrumSeqGate' || name === 'DrumSeqMidi') {
    header();
    u8(2, seq.gate ?? 0);
    const lanes = seq.lanes ?? [];
    if (lanes.length > P.DRUM_SEQ_LANES) throw new Error(`${name}: at most ${P.DRUM_SEQ_LANES} lanes`);
    lanes.forEach((lane, l) => {
      const hits = typeof lane === 'string' ? lane : (lane.hits ?? '');
      const o = typeof lane === 'string' ? {} : lane;
      const base = SEQ_HEADER + l * SEQ_LANE_STRIDE;
      if (name === 'DrumSeqGate') {
        const bits = hitBits(hits);
        u8(base, bits); u8(base + 1, bits >> 8); u8(base + 2, bits >> 16); u8(base + 3, bits >> 24);
        u8(base + 4, o.length ?? 0); u8(base + 5, o.prob ?? 0);
      } else {
        u8(base, o.note ?? 0); u8(base + 1, o.channel ?? 0);
        u8(base + 2, o.length ?? 0); u8(base + 3, o.prob ?? 0);
        const velocities = SEQ_HEADER + P.DRUM_SEQ_LANES * SEQ_LANE_STRIDE + l * P.MAX_SEQUENCE_LEN;
        [...hits].forEach((ch, i) => { if (i < P.MAX_SEQUENCE_LEN) u8(velocities + i, hitVelocity(ch)); });
      }
    });
    return;
  }

  if (name === 'Metronome') {
    // Not a sequencer at all any more - a clock node - but this is where a
    // patch file says what a node's controls mean in words, and "1/8" and
    // "triplet" are exactly that.
    u8(0, fromList(METRONOME_DIVISIONS, seq.division, 'division'));
    u8(1, fromList(METRONOME_FEELS, seq.feel, 'feel'));
    u8(2, seq.width ?? 0);
    return;
  }

  if (GATE_SEQUENCERS.includes(name)) {
    header();
    u8(2, seq.width ?? 0);
    if (name === 'StepSequencer' && seq.hits !== undefined) {
      const bits = hitBits(seq.hits);
      u8(3, bits); u8(4, bits >> 8); u8(5, bits >> 16); u8(6, bits >> 24);
      if (seq.length === undefined) u8(0, String(seq.hits).length);
    }
    if (name === 'EuclidianSequencer') { u8(3, seq.pulses ?? 0); u8(4, seq.rotation ?? 0); }
    if (name === 'RandomSequencer') { u8(3, seq.density ?? 0); u8(4, seq.seed ?? 0); }
    (seq.prob ?? []).forEach((p, i) => u8(8 + i, p));
    return;
  }

  throw new Error(`${name} takes no "seq" block; use "params"`);
}

export function toPatchJson(patch, globals, device) {
  const json = {};

  const jacks = [];
  patch.gatePorts.forEach((port, i) => {
    if (port.direction === P.GatePortDirection.GATE_PORT_UNUSED) return;
    jacks.push({ port: i + 1, dir: DIRECTIONS[port.direction] ?? 'unused', bus: busOrNull(port.bus) });
  });
  if (jacks.length) json.gate_ports = jacks;

  const midiIn = patch.midiIn
    .filter((p) => p.sourceMask)
    .map((p) => ({ sources: portNames(p.sourceMask), channel: p.channel, bus: busOrNull(p.bus) }));
  if (midiIn.length) json.midi_in = midiIn;

  const midiOut = patch.midiOut
    .filter((p) => p.targetMask)
    .map((p) => ({ targets: portNames(p.targetMask), channel: p.channel, bus: busOrNull(p.bus) }));
  if (midiOut.length) json.midi_out = midiOut;

  json.nodes = patch.nodes.map((node) => {
    const d = device?.byId?.get(node.algorithmId);
    const entry = { algo: d ? d.name : node.algorithmId };
    const ins = usedBuses(node.inBus, d ? d.nIn : node.inBus.length);
    const outs = usedBuses(node.outBus, d ? d.nOut : node.outBus.length);
    if (ins.length) entry.in = ins;
    if (outs.length) entry.out = outs;
    const params = usedParams(node.params);
    if (params.length) entry.params = params;
    return entry;
  });

  // The key travels by name, like a sequencer's scale does: a file that says
  // "minor" survives a scale being appended to the firmware's list.
  json.globals = { ...globals, scale: scaleName(globals.scale || P.ScaleId.SCALE_CHROMATIC) };
  const bindings = [];
  patch.ccMap.forEach((m, slot) => {
    if (!m || !m.sourceMask) return;
    bindings.push({ slot, ...m, sources: portNames(m.sourceMask) });
  });
  if (bindings.length) json.cc_map = bindings;

  // Modulation routes, by the name of the parameter they reach rather than
  // only its index: "cutoff" survives a parameter being inserted above it in
  // a later firmware, and is what makes a route in a file readable at all.
  const routes = [];
  (patch.modMap ?? []).forEach((r, slot) => {
    if (!r || r.bus === P.NO_BUS) return;
    const entry = { slot, ...r };
    const named = modParamName(device, patch, r);
    if (named) entry.target = named;
    routes.push(entry);
  });
  if (routes.length) json.mod_map = routes;
  return json;
}

export const toPatchJsonText = (patch, globals, device) =>
  `${JSON.stringify(toPatchJson(patch, globals, device), null, 2)}\n`;

// The way back. A patch written by hand for the emulator is a perfectly good
// starting point for an edit, and without this the JSON would be a one-way
// door out of the app.
//
// Algorithm names are resolved through the device's registry, so this needs a
// module (or the built-in one) attached - the same reason the add-node list
// needs one. A numeric `algo` works either way.
export function fromPatchJson(json, device) {
  if (!json || typeof json !== 'object') throw new Error('that is not a patch object');
  const patch = codec.emptyPatch();
  const globals = { ...codec.emptyGlobals(), ...(json.globals ?? {}) };
  globals.scale = scaleIdOf(globals.scale ?? P.ScaleId.SCALE_CHROMATIC) || P.ScaleId.SCALE_CHROMATIC;
  globals.root = Number(globals.root ?? 0) % 12;

  for (const g of json.gate_ports ?? []) {
    const jack = Number(g.port) - 1;
    if (!(jack >= 0 && jack < patch.gatePorts.length)) {
      throw new Error(`jack ${g.port} does not exist (1..${patch.gatePorts.length})`);
    }
    const direction = Object.entries(DIRECTIONS).find(([, name]) => name === g.dir)?.[0];
    if (direction === undefined) throw new Error(`jack ${g.port}: dir must be in, out or unused`);
    patch.gatePorts[jack] = { direction: Number(direction), bus: g.bus ?? P.NO_BUS };
  }

  (json.midi_in ?? []).forEach((m, i) => {
    if (i >= patch.midiIn.length) throw new Error(`at most ${patch.midiIn.length} MIDI in ports`);
    patch.midiIn[i] = { sourceMask: maskOf(m.sources), channel: m.channel ?? 0, bus: m.bus ?? P.NO_BUS };
  });
  (json.midi_out ?? []).forEach((m, i) => {
    if (i >= patch.midiOut.length) throw new Error(`at most ${patch.midiOut.length} MIDI out ports`);
    patch.midiOut[i] = { targetMask: maskOf(m.targets), channel: m.channel ?? 0, bus: m.bus ?? P.NO_BUS };
  });

  (json.nodes ?? []).forEach((n, i) => {
    const id = resolveAlgorithm(n.algo, device, i);
    const node = codec.emptyNode(id);
    (n.in ?? []).forEach((bus, k) => { if (k < node.inBus.length) node.inBus[k] = bus ?? P.NO_BUS; });
    (n.out ?? []).forEach((bus, k) => { if (k < node.outBus.length) node.outBus[k] = bus ?? P.NO_BUS; });
    (n.params ?? []).forEach((v, k) => { if (k < node.params.length) node.params[k] = v & 0xff; });
    if (n.seq) {
      // Packing needs the algorithm's *name*, since the layout differs between
      // the note, drum and gate sequencers.
      const named = typeof n.algo === 'string'
        ? n.algo
        : device?.byId?.get(id)?.name;
      if (!named) throw new Error(`node ${i}: a "seq" block needs the algorithm by name`);
      packSeq(named, n.seq, node.params);
    }
    patch.nodes.push(node);
  });

  for (const m of json.cc_map ?? []) {
    const slot = Number(m.slot ?? 0);
    if (!(slot >= 0 && slot < patch.ccMap.length)) continue;
    patch.ccMap[slot] = {
      sourceMask: m.sourceMask ?? maskOf(m.sources),
      channel: m.channel ?? 0,
      cc: m.cc ?? 0,
      targetKind: m.targetKind ?? P.CcTargetKind.CC_TARGET_NODE,
      targetIndex: m.targetIndex ?? 0,
      param: m.param ?? 0,
      min: m.min ?? 0,
      max: m.max ?? 0,
      flags: m.flags ?? 0,
    };
  }
  for (const r of json.mod_map ?? []) {
    const slot = Number(r.slot ?? 0);
    if (!(slot >= 0 && slot < patch.modMap.length)) continue;
    // `target` is a name for a reader; `param` is what the module is told.
    // The name is not resolved back - a file whose parameter index and name
    // disagree is a file to fix, not one to guess about.
    patch.modMap[slot] = {
      bus: r.bus ?? P.NO_BUS,
      targetKind: r.targetKind ?? P.CcTargetKind.CC_TARGET_NODE,
      targetIndex: r.targetIndex ?? 0,
      param: r.param ?? 0,
      min: r.min ?? 0,
      max: r.max ?? 0,
      depth: r.depth ?? 255,
      flags: r.flags ?? 0,
    };
  }
  return { patch, globals };
}

// A port by the name a user knows ("DIN 1"), by the firmware's own enum name
// ("SERIAL_1"), or "ALL". Case and spacing do not matter: a patch written by
// hand should not fail over "USB Host" against "USB host".
function maskOf(sources) {
  if (typeof sources === 'number') return sources;
  const names = sources ?? [];
  const all = MIDI_PORTS.reduce((mask, p) => mask | p.value, 0);
  const key = (n) => String(n).toLowerCase().replace(/[\s_]+/g, '');
  let mask = 0;
  const unknown = [];
  for (const name of names) {
    if (key(name) === 'all') { mask |= all; continue; }
    const found = MIDI_PORTS.find((p) => key(p.label) === key(name)
                                      || key(p.key.replace(/^mmMIDI_/, '')) === key(name));
    if (found) mask |= found.value; else unknown.push(name);
  }
  if (unknown.length) throw new Error(`no MIDI port called ${unknown.join(', ')}`);
  return mask;
}

function resolveAlgorithm(algo, device, index) {
  if (typeof algo === 'number') return algo;
  const known = device?.algorithms?.filter(Boolean) ?? [];
  const found = known.find((d) => d.name === algo);
  if (found) return found.id;
  if (!known.length) {
    throw new Error(`node ${index}: "${algo}" needs a module attached to resolve the name — `
                  + 'press "use built-in module" and import again');
  }
  throw new Error(`node ${index}: this firmware has no algorithm "${algo}" `
                + `(it has ${known.map((d) => d.name).join(', ')})`);
}
