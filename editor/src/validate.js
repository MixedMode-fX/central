// The same rules `registry::validate` and `MixedModeMaster::validate` apply,
// so an error surfaces while editing rather than on send.
//
// **An editor that lets you build a patch the module will reject is worse than
// no editor.** These checks are written from the same descriptors the firmware
// publishes - inlet and outlet domains, required inlets, parameter ranges, the
// module's real bus counts - all read from the device, so they cannot drift
// from what the firmware enforces. The test suite sends every patch this
// accepts to the real firmware and asserts it is accepted.

import * as P from './protocol.js';

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

  for (let i = 0; i < descriptor.nIn && i < P.MAX_IN; i++) {
    const bus = node.inBus[i];
    if (bus === P.NO_BUS) {
      if (i < descriptor.minIn) {
        found.push(problem(`${descriptor.name} ${index} inlet ${i}`, 'this inlet must be connected'));
      }
      continue;
    }
    const domain = descriptor.inDomain[i];
    if (bus >= busCount(caps, domain)) {
      found.push(problem(`${descriptor.name} ${index} inlet ${i}`,
        `${domainName(domain)} bus ${bus} does not exist; this module has ${busCount(caps, domain)}`));
    }
  }

  // An outlet left unconnected is normal - a drum sequencer with eight lanes
  // and three jacks patched is the usual case, not an error.
  for (let i = 0; i < descriptor.nOut && i < P.MAX_OUT; i++) {
    const bus = node.outBus[i];
    if (bus === P.NO_BUS) continue;
    const domain = descriptor.outDomain[i];
    if (bus >= busCount(caps, domain)) {
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
  switch (m.targetKind) {
    case P.CcTargetKind.CC_TARGET_NODE: {
      if (m.targetIndex >= patch.nodes.length) {
        found.push(problem(where, `node ${m.targetIndex} is not in this patch`));
        break;
      }
      const node = patch.nodes[m.targetIndex];
      if (!device.describeParam(node.algorithmId, m.param)) {
        found.push(problem(where, `that node has no parameter ${m.param}`));
      }
      break;
    }
    case P.CcTargetKind.CC_TARGET_CLOCK:
      if (m.param >= 3) found.push(problem(where, 'no such clock target'));
      break;
    case P.CcTargetKind.CC_TARGET_TRANSPORT:
      if (m.param >= 4) found.push(problem(where, 'no such transport target'));
      break;
    default:
      found.push(problem(where, 'that target kind is reserved and not built yet'));
  }
  return found;
}

export function validate(device, patch) {
  const caps = device.capabilities;
  const found = [];
  if (!caps) return [problem('editor', 'the module has not reported its capabilities yet')];

  if (patch.nodes.length > caps.nodes) {
    found.push(problem('patch', `${patch.nodes.length} nodes; this module holds ${caps.nodes}`));
  }
  patch.gatePorts.forEach((port, i) => {
    if (port.direction === P.GatePortDirection.GATE_PORT_UNUSED) return;
    if (port.bus >= caps.gateBuses) {
      found.push(problem(`jack ${i + 1}`, `gate bus ${port.bus} does not exist`));
    }
  });
  patch.midiIn.forEach((port, i) => {
    if (!port.sourceMask) return;
    if (port.bus >= caps.noteBuses) found.push(problem(`midi in ${i}`, `note bus ${port.bus} does not exist`));
  });
  patch.midiOut.forEach((port, i) => {
    if (!port.targetMask) return;
    if (port.bus >= caps.noteBuses) found.push(problem(`midi out ${i}`, `note bus ${port.bus} does not exist`));
  });

  patch.nodes.forEach((_, i) => found.push(...validateNode(device, patch, i)));
  patch.ccMap.forEach((_, slot) => found.push(...validateMapping(device, patch, slot)));
  return found;
}

// Warnings are not errors: the firmware runs these patches happily, and an
// editor that refused them would be lying about what the module does. They are
// what a user usually wants to know anyway.
export function advise(device, patch) {
  const notes = [];
  const written = new Set();
  patch.nodes.forEach((node) => {
    const d = device.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (node.outBus[i] !== P.NO_BUS) written.add(`${d.outDomain[i]}:${node.outBus[i]}`);
    }
  });
  patch.gatePorts.forEach((port) => {
    if (port.direction === P.GatePortDirection.GATE_PORT_IN) written.add(`${Domain.Gate}:${port.bus}`);
  });
  patch.midiIn.forEach((port) => { if (port.sourceMask) written.add(`${Domain.Note}:${port.bus}`); });

  patch.nodes.forEach((node, index) => {
    const d = device.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      const bus = node.inBus[i];
      if (bus === P.NO_BUS) continue;
      if (!written.has(`${d.inDomain[i]}:${bus}`)) {
        notes.push(problem(`${d.name} ${index} inlet ${i}`,
          `nothing writes ${domainName(d.inDomain[i])} bus ${bus}`));
      }
    }
  });
  return notes;
}
