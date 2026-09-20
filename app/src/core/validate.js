// The same rules `registry::validate` and `MixedModeMaster::validate` apply,
// so an error surfaces while editing rather than on send.
//
// **An app that lets you build a patch the module will reject is worse than
// no app.** These checks are written from the same descriptors the firmware
// publishes - inlet and outlet domains, required inlets, parameter ranges, the
// module's real bus counts - all read from the device, so they cannot drift
// from what the firmware enforces. The test suite sends every patch this
// accepts to the real firmware and asserts it is accepted.

import * as P from '../protocol/generated.js';
import { isMacro, isDest, isRoute } from './patch.js';

export const Domain = Object.freeze({ Gate: 0, Note: 1, CV: 2 });

export function busCount(capabilities, domain) {
  switch (domain) {
    case Domain.Gate: return capabilities.gateBuses;
    case Domain.Note: return capabilities.noteBuses;
    default: return capabilities.cvBuses;
  }
}

export const domainName = (d) => ['gate', 'note', 'CV'][d] ?? '?';

// One problem, with enough context to fix it.
const problem = (where, message) => ({ where, message });

export function validateNode(device, patch, index) {
  const node = patch.nodes[index];
  const found = [];
  const descriptor = device.byId.get(node.algorithmId);
  if (!descriptor) {
    return [problem(`node ${index}`, `this firmware has no algorithm ${node.algorithmId}`)];
  }
  const caps = device.capabilities;

  // A port names the set of buses it is on, so the question is whether every
  // bus in the set exists: an inlet reading two is as legal as one reading
  // one (src/bus/domain.h).
  for (let i = 0; i < descriptor.nIn && i < P.MAX_IN; i++) {
    const buses = node.inBuses[i] ?? [];
    if (!buses.length) {
      if (i < descriptor.minIn) {
        found.push(problem(`${descriptor.name} ${index} inlet ${i}`, 'this inlet must be connected'));
      }
      continue;
    }
    const domain = descriptor.inDomain[i];
    for (const bus of buses) {
      if (bus < busCount(caps, domain)) continue;
      found.push(problem(`${descriptor.name} ${index} inlet ${i}`,
        `${domainName(domain)} bus ${bus} does not exist; this module has ${busCount(caps, domain)}`));
    }
  }

  // An outlet left unconnected is normal - a drum sequencer with eight lanes
  // and three jacks patched is the usual case, not an error.
  for (let i = 0; i < descriptor.nOut && i < P.MAX_OUT; i++) {
    const domain = descriptor.outDomain[i];
    for (const bus of node.outBuses[i] ?? []) {
      if (bus < busCount(caps, domain)) continue;
      found.push(problem(`${descriptor.name} ${index} outlet ${i}`,
        `${domainName(domain)} bus ${bus} does not exist`));
    }
  }

  // Parameters. Zero is always legal: it means the descriptor's default.
  if (descriptor.params) {
    for (let p = 0; p < descriptor.nParams; p++) {
      const pd = device.describeParam(node.algorithmId, p);
      if (!pd) continue;
      const value = node.params[p];
      if (value === 0) continue;
      if (value < pd.min || value > pd.max) {
        found.push(problem(`${descriptor.name} ${index} "${pd.name}"`,
          `${value} is outside ${pd.min}..${pd.max}`));
      }
    }
  }
  return found;
}

export function validateMapping(device, patch, slot) {
  const m = patch.ccMap[slot];
  if (!m || !m.sourceMask) return [];
  const where = `binding ${slot}`;
  const found = [];
  if (m.cc > 119) found.push(problem(where, 'CC 120..127 are channel mode messages, not controllers'));
  if (m.channel > 16) found.push(problem(where, 'channel must be 0 (omni) or 1..16'));
  if (m.min > m.max) found.push(problem(where, 'the low end of the range is above the high end'));
  if (m.sourceMask & P.MIDI_CONTROL_PORT) {
    found.push(problem(where, 'the control cable is reserved for the protocol'));
  }
  found.push(...targetProblems(device, patch, m, where));
  return found;
}

// Whether a target exists in this patch, by the rules `target_exists` applies
// in src/master.cpp - shared by a binding, a route and a macro destination,
// because they reach one target space and "no such parameter" has to mean the
// same thing to all three. `refuse` names the kinds this particular table may
// not reach at all: a modulator and a macro destination cannot press the
// transport, which is momentary, and a macro destination cannot reach a macro,
// which would be a table writing its own inputs.
function targetProblems(device, patch, entry, where, refuse = []) {
  const found = [];
  if (refuse.includes(entry.targetKind)) {
    found.push(problem(where, entry.targetKind === P.CcTargetKind.CC_TARGET_TRANSPORT
      ? 'the transport is a button, not a value: there is nothing here for this to set'
      : 'a macro cannot reach a macro'));
    return found;
  }
  switch (entry.targetKind) {
    case P.CcTargetKind.CC_TARGET_NODE: {
      if (entry.targetIndex >= patch.nodes.length) {
        found.push(problem(where, `node ${entry.targetIndex} is not in this patch`));
        break;
      }
      const node = patch.nodes[entry.targetIndex];
      if (!device.describeParam(node.algorithmId, entry.param)) {
        found.push(problem(where, `that node has no parameter ${entry.param}`));
      }
      break;
    }
    case P.CcTargetKind.CC_TARGET_CLOCK:
      if (entry.param >= P.CcClockTarget.CC_CLOCK_TARGETS) {
        found.push(problem(where, 'no such clock target'));
      }
      break;
    case P.CcTargetKind.CC_TARGET_TRANSPORT:
      if (entry.param >= P.CcTransportTarget.CC_TRANSPORT_TARGETS) {
        found.push(problem(where, 'no such transport target'));
      }
      break;
    // The key exists whatever the patch holds, and so does every macro slot:
    // a macro is a target because the table has that many entries, not
    // because the patch has put a name in one.
    case P.CcTargetKind.CC_TARGET_KEY:
      if (entry.param >= P.CcKeyTarget.CC_KEY_TARGETS) {
        found.push(problem(where, 'no such key target'));
      }
      break;
    case P.CcTargetKind.CC_TARGET_MACRO:
      if (entry.targetIndex >= (device.capabilities?.macros ?? P.N_MACRO)) {
        found.push(problem(where, `macro ${entry.targetIndex} is not on this module`));
      }
      break;
    default:
      found.push(problem(where, 'that target kind is reserved and not built yet'));
  }
  return found;
}

// One modulation route, against the same rules MixedModeMaster::route_valid
// enforces - so a route the editor accepts is never one the module refuses.
export function validateRoute(device, patch, slot) {
  const r = patch.modMap?.[slot];
  if (!isRoute(r)) return [];
  const where = `modulation ${slot}`;
  const found = [];
  for (const bus of r.buses) {
    if (bus < (device.capabilities?.cvBuses ?? P.N_CV_BUS)) continue;
    found.push(problem(where, `CV bus ${bus} does not exist`));
  }
  if (r.min > r.max) found.push(problem(where, 'the low end of the range is above the high end'));
  // A transport target fires; it does not hold a value, so there is nothing
  // for a continuous signal to set.
  found.push(...targetProblems(device, patch, r, where, [P.CcTargetKind.CC_TARGET_TRANSPORT]));
  // Two writers racing over one value has no defined result. Two modulators
  // reaching one parameter is two writers on one CV bus, which the bus sums.
  const clash = (patch.modMap ?? []).findIndex((other, i) => i !== slot && isRoute(other)
    && other.targetKind === r.targetKind
    && other.targetIndex === r.targetIndex && other.param === r.param);
  if (clash >= 0 && clash < slot) {
    found.push(problem(where, `modulation ${clash} already reaches that parameter — `
                            + 'sum two modulators on one CV bus instead'));
  }
  return found;
}

// One macro destination, against the same rules `MixedModeMaster::dest_valid`
// enforces. A destination belongs to a macro that exists, and reaches a target
// that is neither a macro nor the transport (src/master.cpp).
export function validateMacroDest(device, patch, slot) {
  const d = patch.macroDest?.[slot];
  if (!isDest(d)) return [];
  const where = `macro destination ${slot}`;
  const found = [];
  const macros = device.capabilities?.macros ?? P.N_MACRO;
  if (d.macro >= macros) found.push(problem(where, `macro ${d.macro} is not on this module`));
  found.push(...targetProblems(device, patch, d, where,
    [P.CcTargetKind.CC_TARGET_MACRO, P.CcTargetKind.CC_TARGET_TRANSPORT]));
  return found;
}

export function validate(device, patch) {
  const caps = device.capabilities;
  const found = [];
  if (!caps) return [problem('the app', 'the module has not reported its capabilities yet')];

  if (patch.nodes.length > caps.nodes) {
    found.push(problem('patch', `${patch.nodes.length} nodes; this module holds ${caps.nodes}`));
  }
  const portBuses = (where, buses, limit, domain) => {
    for (const bus of buses ?? []) {
      if (bus < limit) continue;
      found.push(problem(where, `${domainName(domain)} bus ${bus} does not exist`));
    }
  };
  patch.gatePorts.forEach((port, i) => {
    if (port.direction === P.GatePortDirection.GATE_PORT_UNUSED) return;
    portBuses(`jack ${i + 1}`, port.buses, caps.gateBuses, Domain.Gate);
  });
  patch.midiIn.forEach((port, i) => {
    if (port.sourceMask) portBuses(`midi in ${i}`, port.buses, caps.noteBuses, Domain.Note);
  });
  patch.midiOut.forEach((port, i) => {
    if (port.targetMask) portBuses(`midi out ${i}`, port.buses, caps.noteBuses, Domain.Note);
  });

  patch.nodes.forEach((_, i) => found.push(...validateNode(device, patch, i)));
  patch.ccMap.forEach((_, slot) => found.push(...validateMapping(device, patch, slot)));
  (patch.modMap ?? []).forEach((_, slot) => found.push(...validateRoute(device, patch, slot)));
  (patch.macroDest ?? []).forEach((_, slot) => found.push(...validateMacroDest(device, patch, slot)));
  // The pool is shared, so a macro with more destinations than the module
  // expands is refused here rather than by a NAK on the last one written.
  (patch.macros ?? []).forEach((m, index) => {
    if (!isMacro(m)) return;
    const n = (patch.macroDest ?? []).filter((d) => isDest(d) && d.macro === index).length;
    if (n > caps.macroDestsPerMacro) {
      found.push(problem(`macro ${index + 1}`,
        `${n} destinations; one macro holds ${caps.macroDestsPerMacro}`));
    }
  });
  return found;
}

// Warnings are not errors: the firmware runs these patches happily, and an
// app that refused them would be lying about what the module does. They are
// what a user usually wants to know anyway.
export function advise(device, patch) {
  const notes = [];
  const written = new Set();
  patch.nodes.forEach((node) => {
    const d = device.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      for (const bus of node.outBuses[i] ?? []) written.add(`${d.outDomain[i]}:${bus}`);
    }
  });
  patch.gatePorts.forEach((port) => {
    if (port.direction !== P.GatePortDirection.GATE_PORT_IN) return;
    for (const bus of port.buses ?? []) written.add(`${Domain.Gate}:${bus}`);
  });
  patch.midiIn.forEach((port) => {
    if (!port.sourceMask) return;
    for (const bus of port.buses ?? []) written.add(`${Domain.Note}:${bus}`);
  });

  patch.nodes.forEach((node, index) => {
    const d = device.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      for (const bus of node.inBuses[i] ?? []) {
        if (written.has(`${d.inDomain[i]}:${bus}`)) continue;
        notes.push(problem(`${d.name} ${index} inlet ${i}`,
          `nothing writes ${domainName(d.inDomain[i])} bus ${bus}`));
      }
    }
  });

  // A destination on a macro nobody has named is a destination nothing can
  // ever move: the firmware keeps it and simply never expands it, which is
  // why this is a note and not a refusal.
  (patch.macroDest ?? []).forEach((dest, slot) => {
    if (!isDest(dest) || isMacro(patch.macros?.[dest.macro])) return;
    notes.push(problem(`macro destination ${slot}`,
      `macro ${dest.macro + 1} has no name, so nothing can move it`));
  });
  return notes;
}
