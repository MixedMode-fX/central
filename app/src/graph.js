// Patch-shape questions that are not about the DOM: which buses carry a
// signal, and what a node added to this patch should be connected to.
//
// These live apart from the views because they are the part of "add a node"
// that has to be *right*, not merely drawn - and because the app's test
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
// Returns what it did, in words, so the app can say so.
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

// --- the patch as blocks and arrows ---------------------------------------
//
// Everything below answers the questions a *visual* editor asks, and none of
// it touches the DOM: what blocks does this patch have, which arrows run
// between them, and what has to change for a drag from one socket to another
// to become a connection. `canvas.js` draws the answers; `layout.js` places
// them; the tests drive these against the firmware's own validator, because a
// drag that produces a patch the module refuses is the bug that matters here.
//
// **There is no wire in this machine.** An outlet writes a bus and an inlet
// reads one, so an arrow is not a thing a patch stores - it is the observation
// that a writer and a reader are on the same bus. That has three consequences
// the editor has to be honest about, rather than hide behind a cable:
//
//   * one outlet on a bus two inlets read is *two* arrows, and they were made
//     by one bus selection;
//   * two outlets on one bus are two sources merged, which is a real thing to
//     do and is drawn as two arrows arriving at the same socket;
//   * removing one arrow of either shape cannot be done without moving a port
//     off its bus, which changes the other arrows. `planDisconnect` says which.

export const BlockKind = Object.freeze({
  Node: 'node', Jack: 'jack', MidiIn: 'midiIn', MidiOut: 'midiOut',
});

// What a port is called, from the device, falling back to the index for a
// module whose firmware predates port names.
export const inletName = (d, i) => d.inName?.[i] || `in ${i}`;
export const outletName = (d, i) => d.outName?.[i] || `out ${i}`;

const blockId = (kind, index) => `${kind}:${index}`;

// Every block in the patch, in the order a patch stores them: the MIDI inputs
// and the jacks that drive a bus, then the nodes, then what leaves the module.
//
// A jack or a MIDI port that is *unused* is not a block. Eight jacks and eight
// MIDI ports would be sixteen empty boxes around every patch, and they are not
// part of it until something is routed through them - which is what the add
// palette is for.
export function patchBlocks(device, patch) {
  const blocks = [];
  const caps = device?.capabilities;

  patch.midiIn.forEach((port, i) => {
    if (i >= (caps?.midiIn ?? patch.midiIn.length) || !port.sourceMask) return;
    blocks.push({
      id: blockId(BlockKind.MidiIn, i), kind: BlockKind.MidiIn, index: i,
      title: `MIDI in ${i + 1}`, subtitle: 'plays a note bus',
      inlets: [],
      outlets: [{ at: 0, name: 'notes', domain: Domain.Note, bus: port.bus }],
    });
  });

  patch.gatePorts.forEach((port, i) => {
    if (port.direction !== P.GatePortDirection.GATE_PORT_IN) return;
    blocks.push({
      id: blockId(BlockKind.Jack, i), kind: BlockKind.Jack, index: i,
      title: `jack ${i + 1}`, subtitle: 'in — drives a gate bus',
      inlets: [],
      outlets: [{ at: 0, name: 'gate', domain: Domain.Gate, bus: port.bus }],
    });
  });

  patch.nodes.forEach((node, i) => {
    const d = device?.byId.get(node.algorithmId);
    if (!d) {
      blocks.push({
        id: blockId(BlockKind.Node, i), kind: BlockKind.Node, index: i, bad: true,
        title: `node ${i}`, subtitle: `unknown algorithm ${node.algorithmId}`,
        inlets: [], outlets: [],
      });
      return;
    }
    const inlets = [];
    for (let k = 0; k < d.nIn && k < P.MAX_IN; k++) {
      inlets.push({ at: k, name: inletName(d, k), domain: d.inDomain[k],
                    bus: node.inBus[k], required: k < d.minIn });
    }
    const outlets = [];
    for (let k = 0; k < d.nOut && k < P.MAX_OUT; k++) {
      outlets.push({ at: k, name: outletName(d, k), domain: d.outDomain[k], bus: node.outBus[k] });
    }
    blocks.push({
      id: blockId(BlockKind.Node, i), kind: BlockKind.Node, index: i,
      title: d.name, subtitle: d.summary ?? '', clocked: Boolean(d.wantsTick),
      inlets, outlets,
    });
  });

  patch.gatePorts.forEach((port, i) => {
    if (port.direction !== P.GatePortDirection.GATE_PORT_OUT) return;
    blocks.push({
      id: blockId(BlockKind.Jack, i), kind: BlockKind.Jack, index: i,
      title: `jack ${i + 1}`, subtitle: 'out — a gate bus drives it',
      inlets: [{ at: 0, name: 'gate', domain: Domain.Gate, bus: port.bus, required: false }],
      outlets: [],
    });
  });

  patch.midiOut.forEach((port, i) => {
    if (i >= (caps?.midiOut ?? patch.midiOut.length) || !port.targetMask) return;
    blocks.push({
      id: blockId(BlockKind.MidiOut, i), kind: BlockKind.MidiOut, index: i,
      title: `MIDI out ${i + 1}`, subtitle: 'sends a note bus',
      inlets: [{ at: 0, name: 'notes', domain: Domain.Note, bus: port.bus, required: false }],
      outlets: [],
    });
  });

  return blocks;
}

// Every arrow: one per (writer, reader) pair sharing a bus. A bus with two
// writers and three readers is six arrows, which is what that patch does.
export function connectionsOf(blocks) {
  const writers = new Map();   // domain:bus -> [{ block, port }]
  const readers = new Map();
  for (const block of blocks) {
    for (const port of block.outlets) {
      if (port.bus === P.NO_BUS) continue;
      const k = key(port.domain, port.bus);
      if (!writers.has(k)) writers.set(k, []);
      writers.get(k).push({ block, port });
    }
    for (const port of block.inlets) {
      if (port.bus === P.NO_BUS) continue;
      const k = key(port.domain, port.bus);
      if (!readers.has(k)) readers.set(k, []);
      readers.get(k).push({ block, port });
    }
  }
  const arrows = [];
  for (const [k, from] of writers) {
    const to = readers.get(k);
    if (!to) continue;
    const [domain, bus] = k.split(':').map(Number);
    for (const source of from) {
      for (const target of to) {
        arrows.push({
          id: `${source.block.id}#${source.port.at}->${target.block.id}#${target.port.at}`,
          domain, bus,
          from: { blockId: source.block.id, at: source.port.at },
          to: { blockId: target.block.id, at: target.port.at },
          // How many others share this bus, which is what makes removing one
          // arrow a change to the rest.
          writers: from.length, readers: to.length,
        });
      }
    }
  }
  return arrows;
}

// A bus to put a new connection on. One nothing writes *and* nothing reads
// first, so a fresh connection never joins two signals that had nothing to do
// with each other; then one nothing writes, which is a merge nobody asked for
// but is at least audible; then nothing, and the drag is refused.
export function freeBus(blocks, caps, domain) {
  const written = new Set();
  const read = new Set();
  for (const block of blocks) {
    for (const port of block.outlets) if (port.domain === domain) written.add(port.bus);
    for (const port of block.inlets) if (port.domain === domain) read.add(port.bus);
  }
  const n = busCount(caps, domain);
  for (let b = 0; b < n; b++) if (!written.has(b) && !read.has(b)) return b;
  for (let b = 0; b < n; b++) if (!written.has(b)) return b;
  return null;
}

// The bus the last thing to write this domain is on - what a reader added now
// should listen to, and the same rule `connectNewNode` uses: the block added
// most recently, because that is the one somebody has just made.
export function writtenBus(blocks, domain) {
  for (let i = blocks.length - 1; i >= 0; i--) {
    for (const port of blocks[i].outlets) {
      if (port.domain === domain && port.bus !== P.NO_BUS) return port.bus;
    }
  }
  return null;
}

// A bus something is already *waiting* on: read by an inlet and written by
// nothing. Adding a MIDI input after the MIDI output that is to send it should
// feed it, not take a fresh bus and leave the output silent - which is the
// warning `advise` raises about exactly this shape.
export function waitingBus(blocks, domain) {
  const written = new Set();
  const read = new Set();
  for (const block of blocks) {
    for (const port of block.outlets) if (port.domain === domain) written.add(port.bus);
    for (const port of block.inlets) if (port.domain === domain && port.bus !== P.NO_BUS) read.add(port.bus);
  }
  for (const bus of read) if (!written.has(bus)) return bus;
  return null;
}

const findBlock = (blocks, id) => blocks.find((b) => b.id === id) ?? null;

export function portOf(blocks, ref) {
  const block = findBlock(blocks, ref.blockId);
  if (!block) return null;
  const list = ref.isOutlet ? block.outlets : block.inlets;
  const port = list.find((p) => p.at === ref.at);
  return port ? { block, port } : null;
}

// What a drag from one socket to another means, as a list of ports to move and
// a sentence saying what happened. Nothing is applied here: `App.applyPlan`
// writes the patch and sends the messages, so the rule and the effect are not
// the same code.
//
// The bus is the source's, if it has one - dragging *from* something already
// on a bus adds a listener to it rather than moving it, which is what makes
// "one sequencer, three things reading it" the easy shape to build. Otherwise
// the target's, and otherwise a free one. Which end the drag started at does
// not change the answer: dragging an inlet onto an outlet connects the same
// two ports.
export function planConnection(blocks, caps, a, b) {
  const first = portOf(blocks, a);
  const second = portOf(blocks, b);
  if (!first || !second) return { ok: false, why: 'that socket is no longer in the patch' };

  const source = a.isOutlet ? first : second;
  const target = a.isOutlet ? second : first;
  if (Boolean(a.isOutlet) === Boolean(b.isOutlet)) {
    return { ok: false, why: a.isOutlet
      ? 'two outlets cannot be joined — drag an outlet onto an inlet'
      : 'two inlets cannot be joined — an inlet reads a bus, it does not feed one' };
  }
  if (source.port.domain !== target.port.domain) {
    return { ok: false, why: `a ${domainName(source.port.domain)} outlet cannot drive a `
                           + `${domainName(target.port.domain)} inlet` };
  }

  const domain = source.port.domain;
  const writes = [];
  let bus = source.port.bus;
  if (bus === P.NO_BUS) {
    bus = target.port.bus !== P.NO_BUS ? target.port.bus : freeBus(blocks, caps, domain);
    if (bus === null) {
      return { ok: false, why: `every ${domainName(domain)} bus is already written by something` };
    }
    writes.push({ blockId: source.block.id, at: source.port.at, isOutlet: true, bus });
  }
  const was = target.port.bus;
  if (was !== bus) {
    writes.push({ blockId: target.block.id, at: target.port.at, isOutlet: false, bus });
  }
  if (!writes.length) return { ok: false, why: 'those two are already connected' };

  const moved = was !== P.NO_BUS && was !== bus
    ? ` (it was reading ${domainName(domain)} bus ${was})` : '';
  return {
    ok: true, domain, bus, writes,
    said: `${source.block.title} ${source.port.name} → ${target.block.title} ${target.port.name}`
        + ` on ${domainName(domain)} bus ${bus}${moved}`,
  };
}

// A planned write, made to the patch. The *sending* of it is the app's -
// which message a port change is depends on what kind of port it is - but
// which byte moves is a patch-shape question, and it is here so that the tests
// can apply a plan to a patch without a browser and get exactly what the app
// would have got.
export function applyWrite(patch, { blockId, at, isOutlet, bus }) {
  const [kind, where] = blockId.split(':');
  const index = Number(where);
  if (kind === BlockKind.Node) {
    const node = patch.nodes[index];
    if (!node) return null;
    if (isOutlet) node.outBus[at] = bus; else node.inBus[at] = bus;
    return { kind, index, port: node };
  }
  const port = kind === BlockKind.Jack ? patch.gatePorts[index]
    : kind === BlockKind.MidiOut ? patch.midiOut[index]
    : patch.midiIn[index];
  if (!port) return null;
  port.bus = bus;
  return { kind, index, port };
}

// A jack and a MIDI port are only *in* a patch while they are on a bus: the
// module validates the pair, so "in use, but not connected" is not a patch it
// would take. There is therefore no disconnecting one - there is putting it on
// another bus, and there is taking it out of use, which is removing the block.
function endpointRefusal(block) {
  if (block.kind === BlockKind.Node) return null;
  return { ok: false, why: `${block.title} is only in the patch while it is on a bus — `
                         + 'point it at another one, or remove the block to stop using it' };
}

// Removing one arrow. The reader comes off the bus, because that is the end
// the arrow points at and the only end whose other arrows a user is not also
// looking at - but if the bus had more writers, this stops that reader hearing
// all of them, and if it had more readers, they are untouched. Both are said,
// because a patch that quietly loses a connection nobody asked about is worse
// than one that explains itself.
export function planDisconnect(blocks, arrow) {
  const target = portOf(blocks, { ...arrow.to, isOutlet: false });
  if (!target) return { ok: false, why: 'that arrow is no longer in the patch' };
  const refused = endpointRefusal(target.block);
  if (refused) return refused;
  const also = arrow.writers > 1
    ? ` — ${arrow.writers - 1} other source${arrow.writers > 2 ? 's were' : ' was'} on `
      + `${domainName(arrow.domain)} bus ${arrow.bus}, so ${target.block.title} stops hearing `
      + (arrow.writers > 2 ? 'them too' : 'it too')
    : '';
  return {
    ok: true, domain: arrow.domain, bus: arrow.bus,
    writes: [{ blockId: target.block.id, at: target.port.at, isOutlet: false, bus: P.NO_BUS }],
    said: `${target.block.title} ${target.port.name} is not connected${also}`,
  };
}

// Taking one port off its bus, from the socket itself: the same as removing
// every arrow at it at once.
export function planClear(blocks, ref) {
  const found = portOf(blocks, ref);
  if (!found) return { ok: false, why: 'that socket is no longer in the patch' };
  if (found.port.bus === P.NO_BUS) return { ok: false, why: 'that socket is not connected' };
  const refused = endpointRefusal(found.block);
  if (refused) return refused;
  return {
    ok: true, domain: found.port.domain, bus: found.port.bus,
    writes: [{ blockId: found.block.id, at: found.port.at, isOutlet: Boolean(ref.isOutlet), bus: P.NO_BUS }],
    said: `${found.block.title} ${found.port.name} is not connected`,
  };
}
