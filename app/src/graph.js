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

// --- modulation -------------------------------------------------------------
//
// A modulation route is a CV bus reaching a *parameter*
// (src/control/mod_matrix.h), and that is a different kind of thing from
// everything above: a parameter is not a port, it has no domain and no bus,
// and a node has anywhere from two of them to three hundred and thirty-six.
//
// **So a modulated parameter is drawn as an inlet, and an unmodulated one is
// not drawn at all.** Putting every parameter on the block would bury the
// signal path under a wall of sockets - a PolySequencer alone would be 336
// rows - and would say nothing, because a patch is not about the parameters
// nobody has touched. A parameter that something is modulating *is* part of
// the shape of the patch, so it gets a socket; the rest stay in the panel
// below, where they have always been. Dragging a control signal onto a block
// is what turns one of them into the other, and the dropdown that asks which
// one is the editor's way of saying "a parameter is not a port, so you have
// to name it".
//
// A route to the clock has no block to land on - the master clock is not in
// the patch's node list - so it lives in the modulation panel and not on the
// canvas. It is still a route, still validated the same way, and still shown;
// it simply has nowhere to be drawn.
// True for a port synthesised from a modulation route rather than read from a
// descriptor. `at` still numbers the row it is drawn on - `layout.js` places a
// socket from it, so it cannot be an arbitrary handle - but it is *not* an
// index into inBus, and `modSlot` is what says which route the port is.
export const isModPort = (port) => port?.modSlot !== undefined && port?.modSlot !== null;

// Every route that reaches node `index`, as inlet-shaped ports, numbered from
// `firstRow` - they are appended after the algorithm's own inlets, so they
// carry on where those stopped. Ordered by slot, so a block does not
// reshuffle its own sockets when an unrelated route is added.
function modInlets(device, patch, index, firstRow) {
  const ports = [];
  (patch.modMap ?? []).forEach((route, slot) => {
    if (!route || route.bus === P.NO_BUS) return;
    if (route.targetKind !== P.CcTargetKind.CC_TARGET_NODE || route.targetIndex !== index) return;
    ports.push({
      at: firstRow + ports.length, modSlot: slot,
      name: modParamName(device, patch, route),
      domain: Domain.CV, bus: route.bus, required: false,
    });
  });
  return ports;
}

// What a route's target parameter is called, from the device's descriptors -
// the same names the parameter panel draws, so the socket and the control
// below it say the same word.
export function modParamName(device, patch, route) {
  if (route.targetKind === P.CcTargetKind.CC_TARGET_CLOCK) {
    return ['tempo', 'clock source', 'sync ppqn'][route.param] ?? `clock ${route.param}`;
  }
  const node = patch.nodes[route.targetIndex];
  const pd = node ? paramDescriptorOf(device, node.algorithmId, route.param) : null;
  return pd?.name ?? `param ${route.param}`;
}

// One parameter's descriptor. `Device.describeParam` is the lookup - the same
// rule param_lookup() follows in the firmware - and this only tolerates a
// missing device, which the layout tests have.
export const paramDescriptorOf = (device, algorithmId, param) =>
  device?.describeParam?.(algorithmId, param) ?? null;

// The parameters of node `index` a control signal could be pointed at: every
// one the module describes, minus the reserved ones and minus any a route
// already owns - two routes on one parameter is refused by the firmware
// (MixedModeMaster::route_valid), so it is never offered.
export function modulationChoices(device, patch, index) {
  const node = patch.nodes[index];
  const groups = device?.byId.get(node?.algorithmId)?.params;
  if (!node || !groups) return [];
  const taken = new Set((patch.modMap ?? [])
    .filter((r) => r && r.bus !== P.NO_BUS
                && r.targetKind === P.CcTargetKind.CC_TARGET_NODE && r.targetIndex === index)
    .map((r) => r.param));

  const choices = [];
  for (const group of groups) {
    if (!group) continue;
    for (let r = 0; r < group.repeat; r++) {
      for (let f = 0; f < group.nFields; f++) {
        const pd = group.fields[f];
        const param = group.first + r * group.nFields + f;
        if (!pd || (pd.min === 0 && pd.max === 0)) continue;    // reserved
        if (taken.has(param)) continue;
        choices.push({
          param, pd,
          // A table's fields repeat, so "velocity" alone would appear
          // thirty-two times with nothing to tell them apart.
          label: group.repeat > 1 ? `${pd.name} ${r + 1}` : pd.name,
        });
      }
    }
  }
  return choices;
}

// The first modulation slot nothing is using.
export function freeModSlot(patch, limit) {
  const map = patch.modMap ?? [];
  const n = Math.min(limit ?? map.length, map.length);
  for (let i = 0; i < n; i++) if (!map[i] || map[i].bus === P.NO_BUS) return i;
  return null;
}

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
    // Parameters something is modulating, as inlets. See the note above
    // BlockKind: only the ones in use are drawn, which is what keeps a block
    // a block rather than a list of every parameter the algorithm has.
    for (const port of modInlets(device, patch, i, inlets.length)) inlets.push(port);
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

// --- the patch's edges ------------------------------------------------------
//
// A jack and a MIDI port are the two things in a patch that are not
// algorithms, and until now the app made you choose which way each one faced
// *before* you had it: "jack in" and "jack out" were two things to add, and
// changing your mind meant deleting one and adding the other on the same bus.
// A direction is a setting. These two say what changing it means, and like
// every other rule about the shape of a patch they are here rather than in a
// view, so a test can ask them without a browser.

// Which way a jack faces. The bus travels with the turn - a jack in on gate
// bus 3 turned round is a jack out *of* gate bus 3, which is the monitoring
// you were reaching for - and a jack in use has to be on a bus the module
// actually has, so one coming back from unused lands on the bus it last had
// or on the first.
export function planJackDirection(patch, caps, index, direction) {
  const port = patch.gatePorts?.[index];
  if (!port) return null;
  const buses = caps?.gateBuses ?? P.N_GATE_BUS;
  const bus = direction === P.GatePortDirection.GATE_PORT_UNUSED
    ? P.NO_BUS
    : (port.bus === P.NO_BUS || port.bus >= buses ? 0 : port.bus);
  return { index, direction, bus };
}

// Which way a MIDI port faces, which is not a field to write: the module has
// four inputs and four outputs and they are *different ports*
// (src/node/ports.h). So turning one round is moving it - what it carries goes
// to the first free port on the other side, and the one it left goes back to
// unused - and it can fail, when the other side is full.
export function planPortFlip(patch, caps, index, isOut) {
  const port = (isOut ? patch.midiOut : patch.midiIn)?.[index];
  if (!port) return { ok: false, why: 'that port is no longer in the patch' };
  const wantOut = !isOut;
  const to = wantOut ? patch.midiOut : patch.midiIn;
  const limit = (wantOut ? caps?.midiOut : caps?.midiIn) ?? to.length;
  const free = to.findIndex((other, i) =>
    i < limit && !(wantOut ? other.targetMask : other.sourceMask));
  if (free < 0) {
    return { ok: false, why: `every MIDI ${wantOut ? 'output' : 'input'} port is already in use` };
  }
  return {
    ok: true, wantOut,
    from: { index, isOut },
    to: { index: free, isOut: wantOut },
    mask: isOut ? port.targetMask : port.sourceMask,
    channel: port.channel,
    bus: port.bus,
    said: `MIDI ${wantOut ? 'out' : 'in'} ${free + 1}`
        + `${port.bus === P.NO_BUS ? '' : `, note bus ${port.bus}`}`,
  };
}

// The flip, made to the patch. Two ports change: the one being left goes back
// to unused, and the one being taken up gets everything the other carried.
export function applyPortFlip(patch, plan) {
  if (!plan?.ok) return null;
  const from = plan.from.isOut ? patch.midiOut : patch.midiIn;
  const to = plan.to.isOut ? patch.midiOut : patch.midiIn;
  from[plan.from.index] = {
    ...from[plan.from.index],
    [plan.from.isOut ? 'targetMask' : 'sourceMask']: 0,
  };
  to[plan.to.index] = {
    ...to[plan.to.index], channel: plan.channel, bus: plan.bus,
    [plan.to.isOut ? 'targetMask' : 'sourceMask']: plan.mask,
  };
  return plan;
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
      ? 'two outlets cannot be joined'
      : 'two inlets cannot be joined' };
  }
  if (source.port.domain !== target.port.domain) {
    return { ok: false, why: `a ${domainName(source.port.domain)} outlet cannot drive a `
                           + `${domainName(target.port.domain)} inlet` };
  }

  const domain = source.port.domain;
  const writes = [];
  let bus = source.port.bus;
  if (isModPort(source.port)) {
    return { ok: false, why: 'a modulated parameter is a destination, not a source' };
  }
  if (bus === P.NO_BUS) {
    bus = target.port.bus !== P.NO_BUS ? target.port.bus : freeBus(blocks, caps, domain);
    if (bus === null) {
      return { ok: false, why: `every ${domainName(domain)} bus is already written by something` };
    }
    writes.push({ blockId: source.block.id, at: source.port.at, isOutlet: true, bus });
  }
  const was = target.port.bus;
  if (was !== bus) {
    writes.push({ blockId: target.block.id, at: target.port.at, isOutlet: false, bus,
                  modSlot: target.port.modSlot });
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
export function applyWrite(patch, { blockId, at, isOutlet, bus, modSlot }) {
  const [kind, where] = blockId.split(':');
  const index = Number(where);
  // A modulated parameter has no inBus byte behind it: what moves is the
  // route's own bus, and the message that carries it is a different one.
  if (modSlot !== undefined && modSlot !== null) {
    const route = patch.modMap?.[modSlot];
    if (!route) return null;
    if (bus === P.NO_BUS) patch.modMap[modSlot] = null;
    else route.bus = bus;
    return { kind: 'mod', index: modSlot, port: patch.modMap[modSlot] };
  }
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
  return { ok: false, why: `${block.title} is only in the patch while it is on a bus` };
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
      + `${domainName(arrow.domain)} bus ${arrow.bus}`
    : '';
  return {
    ok: true, domain: arrow.domain, bus: arrow.bus,
    writes: [{ blockId: target.block.id, at: target.port.at, isOutlet: false, bus: P.NO_BUS,
               modSlot: target.port.modSlot }],
    said: isModPort(target.port)
      ? `${target.block.title} ${target.port.name} is not modulated any more`
      : `${target.block.title} ${target.port.name} is not connected${also}`,
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
    writes: [{ blockId: found.block.id, at: found.port.at, isOutlet: Boolean(ref.isOutlet),
               bus: P.NO_BUS, modSlot: found.port.modSlot }],
    said: isModPort(found.port)
      ? `${found.block.title} ${found.port.name} is not modulated any more`
      : `${found.block.title} ${found.port.name} is not connected`,
  };
}


// A control signal dropped on a block, once the user has said which parameter
// they meant. The signal's bus is the route's bus - dragging *from* something
// already on a bus adds a listener to it, exactly as `planConnection` does -
// and a source not on a bus yet claims a free one on the way.
//
// Nothing is applied here: `App.applyPlan` writes the patch and sends the
// messages, so the rule and the effect are not the same code.
export function planModulation(blocks, patch, caps, sourceRef, targetBlockId, param, options = {}) {
  const source = portOf(blocks, sourceRef);
  if (!source || !sourceRef.isOutlet) {
    return { ok: false, why: 'a modulation route starts at an outlet' };
  }
  if (source.port.domain !== Domain.CV) {
    return { ok: false, why: `${domainName(source.port.domain)} is not a control signal` };
  }
  const [kind, where] = String(targetBlockId).split(':');
  if (kind !== BlockKind.Node) {
    return { ok: false, why: 'only a node has parameters to modulate' };
  }
  const index = Number(where);
  if (!caps?.modRoutes) {
    return { ok: false, why: 'this firmware has no modulation routes' };
  }
  const slot = freeModSlot(patch, caps.modRoutes);
  if (slot === null) {
    return { ok: false, why: `every one of the module's ${caps.modRoutes} `
                           + 'modulation routes is in use' };
  }

  const writes = [];
  let bus = source.port.bus;
  if (bus === P.NO_BUS) {
    bus = freeBus(blocks, caps, Domain.CV);
    if (bus === null) return { ok: false, why: 'every CV bus is already written by something' };
    writes.push({ blockId: source.block.id, at: source.port.at, isOutlet: true, bus });
  }

  const route = {
    bus,
    targetKind: P.CcTargetKind.CC_TARGET_NODE,
    targetIndex: index,
    param,
    // The target's full range, and the whole of the signal. A route that
    // arrived at some fraction of either would be a route a user has to go
    // and find before it does anything.
    min: 0, max: 0,
    depth: options.depth ?? 255,
    flags: options.flags ?? P.ModFlags.MOD_BIPOLAR | P.ModMode.MOD_OFFSET,
  };
  const target = patch.nodes[index];
  const pd = target ? paramDescriptorOf(options.device, target.algorithmId, param) : null;
  return {
    ok: true, domain: Domain.CV, bus, writes, routes: [{ slot, route }],
    said: `${source.block.title} ${source.port.name} → ${pd?.name ?? `param ${param}`}`
        + ` on CV bus ${bus}`,
  };
}
