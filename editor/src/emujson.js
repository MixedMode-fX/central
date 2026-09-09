// A patch as the emulator's JSON.
//
// `.syx` is the patch *image*: the bytes the module stores, which is what a
// librarian sends and what a preset slot holds. It is not something anyone can
// read, edit or paste, and the full emulator (emulator/index.html) takes a
// patch as JSON - so a patch built here could be sent to a module but not
// tried in the emulator, which is the one place you can actually hear it.
// This is that missing direction.
//
// **The dialect is the emulator's, not a second patch format.** Keys and
// values are exactly what `applyPatch()` in emulator/index.html reads, so the
// output pastes straight into its Patch JSON box. Algorithms are named rather
// than numbered - the emulator resolves names through the same registry - and
// bus indices are per domain, as in NodeConfig.
//
// Two keys the emulator ignores ride along: `globals` and `cc_map`. The
// emulator has its own tempo and clock controls and no controller bindings, so
// it skips anything it does not know; carrying them means an exported patch
// re-imports here without losing what the emulator has no place for. Dropping
// them silently would make the JSON a lossy export of the patch it claims to
// be.

import * as P from './protocol.js';
import * as codec from './codec.js';
import { MIDI_PORTS, portNames, portMaskOf } from './names.js';

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

export function toEmulatorJson(patch, globals, device) {
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

  json.globals = { ...globals };
  const bindings = [];
  patch.ccMap.forEach((m, slot) => {
    if (!m || !m.sourceMask) return;
    bindings.push({ slot, ...m, sources: portNames(m.sourceMask) });
  });
  if (bindings.length) json.cc_map = bindings;
  return json;
}

export const toEmulatorText = (patch, globals, device) =>
  `${JSON.stringify(toEmulatorJson(patch, globals, device), null, 2)}\n`;

// The way back. A patch written by hand for the emulator is a perfectly good
// starting point for an edit, and without this the JSON would be a one-way
// door out of the editor.
//
// Algorithm names are resolved through the device's registry, so this needs a
// module (or the built-in one) attached - the same reason the add-node list
// needs one. A numeric `algo` works either way.
export function fromEmulatorJson(json, device) {
  if (!json || typeof json !== 'object') throw new Error('that is not a patch object');
  const patch = codec.emptyPatch();
  const globals = { ...codec.emptyGlobals(), ...(json.globals ?? {}) };

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
  return { patch, globals };
}

function maskOf(sources) {
  if (typeof sources === 'number') return sources;
  const names = sources ?? [];
  if (names.includes('ALL')) return MIDI_PORTS.reduce((mask, p) => mask | p.value, 0);
  const mask = portMaskOf(names);
  const unknown = names.filter((n) => n !== 'ALL' && !MIDI_PORTS.some((p) => p.label === n));
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
