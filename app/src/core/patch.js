// Questions about a patch that more than one part of the app asks. Pure:
// patch in, answer out, no DOM and no device call.
//
// Every one of these was once written in two or three places - "which route
// reaches this parameter" in the learn button, the plan builder and the block
// list; "who is on this bus" in the port line, the jack card and the MIDI
// card - and the copies were the same until the day one of them was not.

import * as P from '../protocol/generated.js';
import { Domain } from './validate.js';
import { KEY_TARGETS } from '../protocol/names.js';

// A slot in the modulation table holds a route only while it names a bus
// (src/control/mod_matrix.h); a slot in the binding table holds a binding only
// while it names a source cable (src/control/cc_mapper.h). Everything that
// reads either table reads it through these two.
export const isRoute = (r) => Boolean(r) && r.bus !== P.NO_BUS && r.bus !== null && r.bus !== undefined;
export const isBinding = (m) => Boolean(m) && Boolean(m.sourceMask);

const reachesParam = (entry, index, param) =>
  entry.targetKind === P.CcTargetKind.CC_TARGET_NODE
  && entry.targetIndex === index && entry.param === param;

// The route reaching one parameter, with its slot, or null.
export function routeTo(patch, index, param) {
  const slot = (patch.modMap ?? []).findIndex((r) => isRoute(r) && reachesParam(r, index, param));
  return slot < 0 ? null : { slot, ...patch.modMap[slot] };
}

// The binding reaching one parameter, with its slot, or null.
export function bindingTo(patch, index, param) {
  const slot = patch.ccMap.findIndex((m) => isBinding(m) && reachesParam(m, index, param));
  return slot < 0 ? null : { slot, ...patch.ccMap[slot] };
}

// Every route reaching one node, in slot order.
export function routesOf(patch, index) {
  const found = [];
  (patch.modMap ?? []).forEach((route, slot) => {
    if (isRoute(route) && route.targetKind === P.CcTargetKind.CC_TARGET_NODE
        && route.targetIndex === index) {
      found.push({ slot, route });
    }
  });
  return found;
}

// The first slot nothing is using, or null when the table is full.
export function freeModSlot(patch, limit) {
  const map = patch.modMap ?? [];
  const n = Math.min(limit ?? map.length, map.length);
  for (let i = 0; i < n; i++) if (!isRoute(map[i])) return i;
  return null;
}

export function freeCcSlot(patch, limit) {
  const map = patch.ccMap ?? [];
  const n = Math.min(limit ?? map.length, map.length);
  for (let i = 0; i < n; i++) if (!isBinding(map[i])) return i;
  return null;
}

// What a port is called, from the device, falling back to the index.
export const inletName = (d, i) => d.inName?.[i] || `in ${i}`;
export const outletName = (d, i) => d.outName?.[i] || `out ${i}`;

// One parameter's descriptor, tolerating a missing device (the layout tests
// have none).
export const paramDescriptorOf = (device, algorithmId, param) =>
  device?.describeParam?.(algorithmId, param) ?? null;

// What a route's or a binding's target parameter is called, from the device's
// descriptors - the same names the parameter panel draws, so a socket and the
// control below it say the same word.
export function targetParamName(device, patch, entry) {
  if (entry.targetKind === P.CcTargetKind.CC_TARGET_CLOCK) {
    return ['tempo', 'clock source', 'sync ppqn'][entry.param] ?? `clock ${entry.param}`;
  }
  if (entry.targetKind === P.CcTargetKind.CC_TARGET_KEY) {
    return KEY_TARGETS.find((t) => t.value === entry.param)?.label ?? `key ${entry.param}`;
  }
  const node = patch.nodes[entry.targetIndex];
  const pd = node ? paramDescriptorOf(device, node.algorithmId, entry.param) : null;
  return pd?.name ?? `param ${entry.param}`;
}

// The header parameters of a descriptor - the ones a knob can reach, which
// leaves out a table's repeating fields and the reserved bytes - as
// `{ at, pd }`, in the firmware's order.
export function knobParams(descriptor) {
  const found = [];
  for (const group of descriptor?.params ?? []) {
    if (!group || group.repeat > 1) continue;
    for (let f = 0; f < group.nFields; f++) {
      const pd = group.fields[f];
      if (!pd || (pd.min === 0 && pd.max === 0)) continue;
      found.push({ at: group.first + f, pd });
    }
  }
  return found;
}

// Where a parameter lives on a node, by the name the firmware gave it: the
// one thing the app may look a parameter up by.
export const paramNamed = (descriptor, name) =>
  knobParams(descriptor).find(({ pd }) => pd.name === name) ?? null;

// What a stored byte is worth: zero means the descriptor's default everywhere
// (src/node/param.h).
export const effectiveValue = (pd, stored) => (stored === 0 ? pd.def : stored);

// --- renumbering ------------------------------------------------------------

// Removing node `removed` renumbers every node after it. Everything addressed
// by node index - a binding's target, a route's target, a block's position, a
// drum voice - moves by this one rule: gone, the same, or one less.
export function shiftNodeIndex(index, removed) {
  if (index === removed) return null;
  return index > removed ? index - 1 : index;
}

// The bindings and routes of a patch, with node `removed` taken out: those
// pointing at it are dropped rather than silently rebound, the rest move.
export function renumberTargets(entries, removed) {
  return entries.map((entry) => {
    if (!entry || entry.targetKind !== P.CcTargetKind.CC_TARGET_NODE) return entry;
    const targetIndex = shiftNodeIndex(entry.targetIndex, removed);
    return targetIndex === null ? null : { ...entry, targetIndex };
  });
}

// A map keyed `node:N` (block positions, drum voices), renumbered the same
// way. Keys that are not a node are untouched.
export function renumberNodeKeys(entries, removed) {
  const moved = new Map();
  for (const [key, value] of entries) {
    const match = /^node:(\d+)$/.exec(key);
    if (!match) { moved.set(key, value); continue; }
    const index = shiftNodeIndex(Number(match[1]), removed);
    if (index !== null) moved.set(`node:${index}`, value);
  }
  return moved;
}

// --- who is on a bus ---------------------------------------------------------

// Every bus something writes, as `domain:bus` keys: the set an inlet can
// usefully be pointed at.
export function writtenBuses(device, patch) {
  const written = new Set();
  for (const peer of everyPeer(device, patch)) {
    if (peer.writes) written.add(`${peer.domain}:${peer.bus}`);
  }
  return written;
}

// Every port of the patch that is on a bus, whichever kind of block it
// belongs to: a node's inlet or outlet, a jack, a MIDI port. Each carries an
// id a caller can recognise itself by.
function* everyPeer(device, patch) {
  for (const [index, node] of patch.nodes.entries()) {
    const d = device?.byId.get(node.algorithmId);
    if (!d) continue;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (node.outBus[i] === P.NO_BUS) continue;
      yield { id: `node:${index}#out${i}`, label: `${d.name} ${index} ${outletName(d, i)}`,
              domain: d.outDomain[i], bus: node.outBus[i], writes: true };
    }
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      if (node.inBus[i] === P.NO_BUS) continue;
      yield { id: `node:${index}#in${i}`, label: `${d.name} ${index} ${inletName(d, i)}`,
              domain: d.inDomain[i], bus: node.inBus[i], writes: false };
    }
  }
  for (const [i, port] of patch.gatePorts.entries()) {
    if (port.direction === P.GatePortDirection.GATE_PORT_UNUSED || port.bus === P.NO_BUS) continue;
    yield { id: `jack:${i}`, label: `jack ${i + 1}`, domain: Domain.Gate, bus: port.bus,
            writes: port.direction === P.GatePortDirection.GATE_PORT_IN };
  }
  for (const [i, port] of patch.midiIn.entries()) {
    if (!port.sourceMask || port.bus === P.NO_BUS) continue;
    yield { id: `midiIn:${i}`, label: `MIDI in ${i + 1}`, domain: Domain.Note, bus: port.bus, writes: true };
  }
  for (const [i, port] of patch.midiOut.entries()) {
    if (!port.targetMask || port.bus === P.NO_BUS) continue;
    yield { id: `midiOut:${i}`, label: `MIDI out ${i + 1}`, domain: Domain.Note, bus: port.bus, writes: false };
  }
}

// Who writes and who reads one bus, in words. A bus *is* the connection, so
// the thing a patch cable would have shown - what this inlet is actually
// listening to - has to be said. `self` is the asking port's own id, left out
// of its own answer.
export function busPeers(device, patch, domain, bus, self = null) {
  const writers = [];
  const readers = [];
  if (bus === P.NO_BUS) return { writers, readers };
  for (const peer of everyPeer(device, patch)) {
    if (peer.domain !== domain || peer.bus !== bus || peer.id === self) continue;
    (peer.writes ? writers : readers).push(peer.label);
  }
  return { writers, readers };
}

// The buses a domain uses anywhere in the patch: a port on it, or a route
// reading it.
export function usedBuses(device, patch, domain) {
  const used = new Set();
  for (const peer of everyPeer(device, patch)) if (peer.domain === domain) used.add(peer.bus);
  if (domain === Domain.CV) {
    for (const route of patch.modMap ?? []) if (isRoute(route)) used.add(route.bus);
  }
  return used;
}
