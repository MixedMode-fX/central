// Patch-shape questions that are not about the DOM: which buses carry a
// signal, and what a node added to this patch should be connected to.
//
// These live apart from the views because they are the part of "add a node"
// that has to be *right*, not merely drawn - and because the editor's test
// suite drives them against the real firmware's validator, where a mistake
// shows up as a rejected patch rather than as a layout that looks odd.

import * as P from './protocol.js';
import { Domain, busCount, domainName } from './validate.js';

const key = (domain, bus) => `${domain}:${bus}`;

// Every bus something writes to. A bus is the connection, so this is the set
// an inlet can usefully be pointed at.
export function writtenBuses(device, patch) {
  const written = new Set();
  patch.nodes.forEach((node) => {
    const d = device?.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (node.outBus[i] !== P.NO_BUS) written.add(key(d.outDomain[i], node.outBus[i]));
    }
  });
  patch.gatePorts.forEach((port) => {
    if (port.direction === P.GatePortDirection.GATE_PORT_IN && port.bus !== P.NO_BUS) {
      written.add(key(Domain.Gate, port.bus));
    }
  });
  patch.midiIn.forEach((port) => {
    if (port.sourceMask && port.bus !== P.NO_BUS) written.add(key(Domain.Note, port.bus));
  });
  return written;
}

// A node added with every port unconnected is a node the module refuses: its
// required inlets are empty, so the patch stops being sendable the moment you
// add it, and every incremental edit afterwards addresses a node the module
// never took. "Add a clock, add an inverter" hit exactly that, and reported it
// as a bad argument.
//
// So a new node arrives patched. Required inlets go to a bus something already
// writes - which is what makes the second node of a chain land on the first -
// and the first outlet to a bus nothing writes yet, so two sources do not end
// up merged by accident. Both are ordinary bus selections shown in the node's
// own panel: nothing here is hidden, and every one of them can be changed.
//
// Returns what it did, in words, so the editor can say so.
export function connectNewNode(device, patch, node, descriptor) {
  const caps = device?.capabilities;
  if (!caps) return [];
  const written = writtenBuses(device, patch);
  const said = [];

  for (let i = 0; i < descriptor.minIn && i < descriptor.nIn && i < P.MAX_IN; i++) {
    const domain = descriptor.inDomain[i];
    // The node most recently added first: "add a divider, add a sequencer"
    // means the sequencer reads the divider, not whichever bus a jack happened
    // to claim earlier. Then any bus something writes, then bus 0.
    let bus = latestOutlet(device, patch, domain);
    if (bus === null) {
      bus = 0;
      for (let b = 0; b < busCount(caps, domain); b++) {
        if (written.has(key(domain, b))) { bus = b; break; }
      }
    }
    node.inBus[i] = bus;
    said.push(`${descriptor.inName?.[i] || `in ${i}`} on ${domainName(domain)} bus ${bus}`);
  }

  if (descriptor.nOut > 0) {
    const domain = descriptor.outDomain[0];
    for (let b = 0; b < busCount(caps, domain); b++) {
      if (written.has(key(domain, b))) continue;
      node.outBus[0] = b;
      said.push(`${descriptor.outName?.[0] || 'out 0'} on ${domainName(domain)} bus ${b}`);
      break;
    }
  }
  return said;
}

// The last node in the patch that writes this domain, and the bus it writes.
// Nodes are added in order, so the last one is the one a user just made.
function latestOutlet(device, patch, domain) {
  for (let n = patch.nodes.length - 1; n >= 0; n--) {
    const d = device?.byId.get(patch.nodes[n].algorithmId);
    if (!d) continue;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (d.outDomain[i] === domain && patch.nodes[n].outBus[i] !== P.NO_BUS) {
        return patch.nodes[n].outBus[i];
      }
    }
  }
  return null;
}
