// The patch's shape, as an editor sees it: the blocks and arrows a canvas
// draws, and what a drag from one socket to another means.
//
// These live apart from the views because they are the part of a patch edit
// that has to be *right*, not merely drawn - and because the app's test
// suite drives them against the real firmware's validator, where a mistake
// shows up as a rejected patch rather than as a layout that looks odd.

import * as P from '../protocol/generated.js';
import { Domain, busCount, domainName } from './validate.js';
import { emptyNode } from '../protocol/codec.js';
import { MUSICAL_PORTS, ALL_MUSICAL } from '../protocol/names.js';
import {
  isBinding, routesOf, routeTo, bindingTo, freeModSlot, freeCcSlot,
  inletName, outletName, paramDescriptorOf, targetParamName,
} from './patch.js';

const key = (domain, bus) => `${domain}:${bus}`;

// **Every block arrives unconnected, and nothing here chooses a bus for one.**
// A bus chosen for you is a wire you did not draw. The editor used to patch a
// new node in on arrival - required inlets onto the bus the last node wrote,
// the first outlet onto the first bus nothing wrote yet - and every one of
// those choices was a guess about a patch that did not exist yet. Add a MIDI
// output while a sequencer is being built and the sequencer's next node lands
// on the output's bus; add two unrelated nodes and they come out chained. On
// a canvas those guesses are arrows nobody drew.
//
// So a node, a jack and a MIDI port all arrive on no bus at all, and the
// wires are the ones you drag. Until a required inlet has one the patch is
// incomplete and the app says so rather than sending it (`validate`,
// `Editor.sendWhole`) - which is the same thing the canvas is already showing.
// The only bus this file ever picks is the one a *source* claims on the drag
// that first gives it something to say: `freeBus`.

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
//   * one outlet on a bus two inlets read is *two* arrows;
//   * two writers of one bus are two sources merged, and so are two buses in
//     one inlet's set: both are drawn as two arrows arriving at one socket;
//   * a port is on a *set* of buses, so an arrow can be removed on its own -
//     the reader drops that one bus and keeps the rest.
//
// That last one is why a port carries a set rather than a bus. With one bus
// per port, summing two sources meant putting them on the same bus, which
// merged everything else they were driving too: connecting one thing
// disconnected another. Now a source keeps its own bus and a destination
// listens to as many as it likes.

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
// index into inBuses, and `modSlot` is what says which route the port is.
export const isModPort = (port) => port?.modSlot !== undefined && port?.modSlot !== null;

// Every route that reaches node `index`, as inlet-shaped ports, numbered from
// `firstRow` - they are appended after the algorithm's own inlets, so they
// carry on where those stopped. Ordered by slot, so a block does not
// reshuffle its own sockets when an unrelated route is added.
function modInlets(device, patch, index, firstRow) {
  return routesOf(patch, index).map(({ slot, route }, row) => ({
    at: firstRow + row, modSlot: slot,
    name: targetParamName(device, patch, route),
    domain: Domain.CV, buses: route.buses, required: false,
  }));
}

// The parameters of node `index` a control signal could be pointed at: every
// one the module describes, minus the reserved ones and minus any a route
// already owns - two routes on one parameter is refused by the firmware
// (MixedModeMaster::route_valid), so it is never offered.
export function modulationChoices(device, patch, index) {
  const node = patch.nodes[index];
  const groups = device?.byId.get(node?.algorithmId)?.params;
  if (!node || !groups) return [];
  const taken = new Set(routesOf(patch, index).map(({ route }) => route.param));

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
      outlets: [{ at: 0, name: 'notes', domain: Domain.Note, buses: port.buses }],
    });
  });

  patch.gatePorts.forEach((port, i) => {
    if (port.direction !== P.GatePortDirection.GATE_PORT_IN) return;
    blocks.push({
      id: blockId(BlockKind.Jack, i), kind: BlockKind.Jack, index: i,
      title: `jack ${i + 1}`, subtitle: 'in — drives a gate bus',
      inlets: [],
      outlets: [{ at: 0, name: 'gate', domain: Domain.Gate, buses: port.buses }],
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
                    buses: node.inBuses[k] ?? [], required: k < d.minIn });
    }
    // Parameters something is modulating, as inlets. See the note above
    // BlockKind: only the ones in use are drawn, which is what keeps a block
    // a block rather than a list of every parameter the algorithm has.
    for (const port of modInlets(device, patch, i, inlets.length)) inlets.push(port);
    const outlets = [];
    for (let k = 0; k < d.nOut && k < P.MAX_OUT; k++) {
      outlets.push({ at: k, name: outletName(d, k), domain: d.outDomain[k],
                     buses: node.outBuses[k] ?? [] });
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
      inlets: [{ at: 0, name: 'gate', domain: Domain.Gate, buses: port.buses, required: false }],
      outlets: [],
    });
  });

  patch.midiOut.forEach((port, i) => {
    if (i >= (caps?.midiOut ?? patch.midiOut.length) || !port.targetMask) return;
    blocks.push({
      id: blockId(BlockKind.MidiOut, i), kind: BlockKind.MidiOut, index: i,
      title: `MIDI out ${i + 1}`, subtitle: 'sends a note bus',
      inlets: [{ at: 0, name: 'notes', domain: Domain.Note, buses: port.buses, required: false }],
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
      for (const bus of port.buses) {
        const k = key(port.domain, bus);
        if (!writers.has(k)) writers.set(k, []);
        writers.get(k).push({ block, port });
      }
    }
    for (const port of block.inlets) {
      for (const bus of port.buses) {
        const k = key(port.domain, bus);
        if (!readers.has(k)) readers.set(k, []);
        readers.get(k).push({ block, port });
      }
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
          id: `${source.block.id}#${source.port.at}->${target.block.id}#${target.port.at}@${domain}:${bus}`,
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

// Which way a jack faces. The buses travel with the turn - a jack in on gate
// bus 3 turned round is a jack out *of* gate bus 3, which is the monitoring
// you were reaching for - and a jack taken out of use lets go of them, so
// putting it back does not silently reconnect it.
export function planJackDirection(patch, caps, index, direction) {
  const port = patch.gatePorts?.[index];
  if (!port) return null;
  const limit = caps?.gateBuses ?? P.N_GATE_BUS;
  const buses = direction === P.GatePortDirection.GATE_PORT_UNUSED
    ? []
    : (port.buses ?? []).filter((b) => b < limit);
  return { index, direction, buses };
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
    buses: port.buses ?? [],
    said: `MIDI ${wantOut ? 'out' : 'in'} ${free + 1}`
        + ((port.buses ?? []).length ? `, note ${busWords(port.buses)}` : ''),
  };
}

// **Fanning an output out: the same note buses, a second cable and a second
// channel.** One output already sends to every cable in its mask, so this is
// not about reaching more of them - it is about reaching one of them on a
// *different channel*, which is a second port or nothing.
//
// There is no such action on an input. An input reaching a second note bus is
// that bus in its own set now (src/bus/domain.h), which is a drag on the
// canvas rather than a second port to keep in step.
export function planPortFanOut(patch, caps, index) {
  const ports = patch.midiOut ?? [];
  const port = ports[index];
  if (!port || !port.targetMask) return { ok: false, why: 'that port is not in use' };
  const limit = caps?.midiOut ?? ports.length;
  const free = ports.findIndex((other, i) => i < limit && !other.targetMask);
  if (free < 0) return { ok: false, why: 'every MIDI output port is already in use' };
  if (!(port.buses ?? []).length) return { ok: false, why: 'that port is on no note bus' };

  // Every cable those buses already go down, so the copy lands on one they do
  // not: a second output doubling the first is not a fan-out, it is a flam.
  let taken = 0;
  ports.forEach((other, i) => {
    if (i < limit && other.targetMask && sameBuses(other.buses, port.buses)) taken |= other.targetMask;
  });
  const cable = MUSICAL_PORTS.find((p) => (taken & p.value) === 0);
  if (!cable) return { ok: false, why: 'those note buses already play every cable' };
  return {
    ok: true, index: free, isOut: true, mask: cable.value, channel: port.channel,
    buses: port.buses,
    said: `MIDI out ${free + 1} plays note ${busWords(port.buses)} on ${cable.label} too`,
  };
}

const sameBuses = (a, b) => (a ?? []).length === (b ?? []).length
  && (a ?? []).every((bus) => b.includes(bus));

// A port's buses in words: "bus 3", or "buses 0 and 3" for a merge.
export function busWords(buses) {
  const list = buses ?? [];
  if (!list.length) return 'no bus';
  if (list.length === 1) return `bus ${list[0]}`;
  return `buses ${list.slice(0, -1).join(', ')} and ${list[list.length - 1]}`;
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
    ...to[plan.to.index], channel: plan.channel, buses: plan.buses,
    [plan.to.isOut ? 'targetMask' : 'sourceMask']: plan.mask,
  };
  return plan;
}

// A bus for a source that has none yet. One nothing writes *and* nothing
// reads first, so a fresh connection never joins two signals that had nothing
// to do with each other; then one nothing writes, which is a merge nobody
// asked for but is at least audible; then nothing, and the drag is refused.
export function freeBus(blocks, caps, domain) {
  const written = new Set();
  const read = new Set();
  for (const block of blocks) {
    for (const port of block.outlets) if (port.domain === domain) for (const b of port.buses) written.add(b);
    for (const port of block.inlets) if (port.domain === domain) for (const b of port.buses) read.add(b);
  }
  const n = busCount(caps, domain);
  for (let b = 0; b < n; b++) if (!written.has(b) && !read.has(b)) return b;
  for (let b = 0; b < n; b++) if (!written.has(b)) return b;
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

// What a drag from one socket to another means, as a list of ports to write
// and a sentence saying what happened. Nothing is applied here:
// `App.applyPlan` writes the patch and sends the messages, so the rule and the
// effect are not the same code.
//
// **A connection only ever adds.** The source keeps its bus - claiming a free
// one if this is the first thing it has been asked to drive - and the target
// adds that bus to the set it already listens to. So a source reaches as many
// destinations as you drag it to, a destination sums as many sources as you
// drag into it, and neither ever costs the other a connection it already had.
// Which end the drag started at does not change the answer.
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
  if (isModPort(source.port)) {
    return { ok: false, why: 'a modulated parameter is a destination, not a source' };
  }

  const domain = source.port.domain;
  const writes = [];
  // The source's own bus. One already on several is asked to speak on the
  // first of them: that is the bus that *is* this source in the patch.
  let bus = source.port.buses[0];
  if (bus === undefined) {
    bus = freeBus(blocks, caps, domain);
    if (bus === null) {
      return { ok: false, why: `every ${domainName(domain)} bus is already written by something` };
    }
    writes.push({ blockId: source.block.id, at: source.port.at, isOutlet: true, buses: [bus] });
  }
  if (target.port.buses.includes(bus)) {
    return { ok: false, why: 'those two are already connected' };
  }
  const buses = [...target.port.buses, bus].sort((x, y) => x - y);
  writes.push({ blockId: target.block.id, at: target.port.at, isOutlet: false, buses,
                modSlot: target.port.modSlot });

  const also = target.port.buses.length
    ? `, summed with ${busWords(target.port.buses)}` : '';
  return {
    ok: true, domain, bus, writes,
    said: `${source.block.title} ${source.port.name} → ${target.block.title} ${target.port.name}`
        + ` on ${domainName(domain)} bus ${bus}${also}`,
  };
}

// A planned write, made to the patch. The *sending* of it is the app's -
// which message a port change is depends on what kind of port it is - but
// which byte moves is a patch-shape question, and it is here so that the tests
// can apply a plan to a patch without a browser and get exactly what the app
// would have got.
export function applyWrite(patch, { blockId, at, isOutlet, buses, modSlot }) {
  const [kind, where] = blockId.split(':');
  const index = Number(where);
  const set = [...(buses ?? [])].sort((x, y) => x - y);
  // A modulated parameter has no inlet behind it: what moves is the route's
  // own source buses, and the message that carries it is a different one.
  if (modSlot !== undefined && modSlot !== null) {
    const route = patch.modMap?.[modSlot];
    if (!route) return null;
    if (!set.length) patch.modMap[modSlot] = null;
    else route.buses = set;
    return { kind: 'mod', index: modSlot, port: patch.modMap[modSlot] };
  }
  if (kind === BlockKind.Node) {
    const node = patch.nodes[index];
    if (!node) return null;
    if (isOutlet) node.outBuses[at] = set; else node.inBuses[at] = set;
    return { kind, index, port: node };
  }
  const port = kind === BlockKind.Jack ? patch.gatePorts[index]
    : kind === BlockKind.MidiOut ? patch.midiOut[index]
    : patch.midiIn[index];
  if (!port) return null;
  port.buses = set;
  return { kind, index, port };
}

// Removing one arrow, and only that one. The reader drops the arrow's bus
// from its set and keeps every other bus it was listening to; the writer and
// its other readers are untouched. That is the whole of it - which is what a
// set of buses per port buys, and why this no longer has to explain what else
// it took away.
export function planDisconnect(blocks, arrow) {
  const target = portOf(blocks, { ...arrow.to, isOutlet: false });
  if (!target) return { ok: false, why: 'that arrow is no longer in the patch' };
  const buses = target.port.buses.filter((b) => b !== arrow.bus);
  const left = buses.length ? `, still reading ${busWords(buses)}` : '';
  return {
    ok: true, domain: arrow.domain, bus: arrow.bus,
    writes: [{ blockId: target.block.id, at: target.port.at, isOutlet: false, buses,
               modSlot: target.port.modSlot }],
    said: isModPort(target.port) && !buses.length
      ? `${target.block.title} ${target.port.name} is not modulated any more`
      : `${target.block.title} ${target.port.name} is off `
        + `${domainName(arrow.domain)} bus ${arrow.bus}${left}`,
  };
}

// --- copying a node ---------------------------------------------------------
//
// A copy is a node's *settings*: the algorithm it runs, every parameter byte
// it holds, and the buses its inlets read. **Its outlets arrive empty**, for
// the reason at the top of this file - a bus chosen for you is a wire you did
// not draw. Reading a bus a second time is fan-out, which is the ordinary case
// and changes nothing that was already playing; writing one a second time is a
// merge, and a merge nobody drew is a sound nobody asked for. So a duplicated
// sequencer is still advanced by whatever advanced the one it came from, and
// says nothing at all until it is given a bus of its own.
//
// What is *not* copied is everything addressed by node index from outside the
// node: the routes modulating it and the controller bindings pointing at it
// stay with the original. A parameter reachable from two knobs because a
// block was duplicated is a patch nobody can read.
export function copyNode(patch, index) {
  const node = patch.nodes[index];
  if (!node) return null;
  return {
    algorithmId: node.algorithmId,
    inBuses: (node.inBuses ?? []).map((buses) => [...(buses ?? [])]),
    params: Uint8Array.from(node.params),
  };
}

// A copy, as a node this module could hold: null when it does not run that
// algorithm at all. A bus the copy read that this module has not got is
// dropped rather than carried - the clipboard outlives the patch it was taken
// from, and a module with four gate buses cannot read the eighth.
export function nodeFromCopy(device, copy) {
  const d = device?.byId?.get(copy?.algorithmId);
  if (!d) return null;
  const caps = device.capabilities;
  const node = emptyNode(copy.algorithmId);
  node.params.set(copy.params.subarray(0, node.params.length));
  for (let k = 0; k < d.nIn && k < P.MAX_IN; k++) {
    const buses = copy.inBuses[k] ?? [];
    node.inBuses[k] = buses.filter((bus) => bus < busCount(caps, d.inDomain[k]));
  }
  return node;
}

// Taking one port off every bus it is on, from the socket itself: the same as
// removing every arrow at it at once.
export function planClear(blocks, ref) {
  const found = portOf(blocks, ref);
  if (!found) return { ok: false, why: 'that socket is no longer in the patch' };
  if (!found.port.buses.length) return { ok: false, why: 'that socket is not connected' };
  return {
    ok: true, domain: found.port.domain, buses: found.port.buses,
    writes: [{ blockId: found.block.id, at: found.port.at, isOutlet: Boolean(ref.isOutlet),
               buses: [], modSlot: found.port.modSlot }],
    said: isModPort(found.port)
      ? `${found.block.title} ${found.port.name} is not modulated any more`
      : `${found.block.title} ${found.port.name} is not connected`,
  };
}


// A control signal dropped on a block, once the user has said which parameter
// they meant. The signal's bus becomes the route's source, exactly as
// `planConnection` does - and a source not on a bus yet claims a free one on
// the way. A second signal is summed into the same route by dragging onto the
// socket it drew, like any other inlet.
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
  let bus = source.port.buses[0];
  if (bus === undefined) {
    bus = freeBus(blocks, caps, Domain.CV);
    if (bus === null) return { ok: false, why: 'every CV bus is already written by something' };
    writes.push({ blockId: source.block.id, at: source.port.at, isOutlet: true, buses: [bus] });
  }

  const route = {
    buses: [bus],
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

// The CC numbers a binding may name. 120 and above are channel mode messages
// - all notes off, local control - not controls, and a 14-bit binding reads
// CC n+32 as the low half of n.
export const CC_MAX = 119;

// A controller's CC onto a parameter, made from the parameter's side: "bind
// this to CC n", which is what the learn button's menu asks when a number is
// picked instead of a knob turned. It is the same row a learn writes - the
// firmware fills in the port and the channel it saw the CC arrive on - so the
// two ways of making a binding cannot disagree about what a new one is.
//
// A parameter already bound changes number in the slot it has, keeping its
// channel, its ports, its range, its takeover and its polarity: a binding is
// edited, not replaced. A new one listens on every musical cable and every
// channel, because a patch built with nothing plugged in cannot know which
// cable the controller will arrive on, and the mod matrix narrows it once it
// is there.
export function planCcBinding(patch, caps, index, param, cc, options = {}) {
  if (!Number.isInteger(cc) || cc < 0 || cc > CC_MAX) {
    return { ok: false, why: `CC ${cc} is not a control the module binds` };
  }
  const target = patch.nodes[index];
  if (!target) return { ok: false, why: 'that node is no longer in the patch' };
  const pd = paramDescriptorOf(options.device, target.algorithmId, param);
  const name = pd?.name ?? `param ${param}`;

  const existing = bindingTo(patch, index, param);
  if (existing) {
    const { slot: at, ...mapping } = existing;
    return { ok: true, writes: [], bindings: [{ slot: at, mapping: { ...mapping, cc } }],
             said: `${name} is on CC ${cc}` };
  }
  const slot = freeCcSlot(patch, caps?.ccMappings);
  if (slot === null) {
    return { ok: false, why: `every one of the module's ${caps?.ccMappings ?? P.N_CC_MAP} `
                           + 'controller bindings is in use' };
  }
  const mapping = {
    sourceMask: ALL_MUSICAL, channel: 0, cc,
    targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: index, param,
    // The target's full range, taken from the descriptors by the firmware, and
    // a knob that jumps: a binding that arrived needing to be set up before it
    // moved anything is a binding nobody made.
    min: 0, max: 0, flags: 0,
  };
  return { ok: true, writes: [], bindings: [{ slot, mapping }], said: `CC ${cc} → ${name}` };
}

// The same route, made from the parameter's side: "modulate this from CV bus
// N", which is what the CV button beside every control asks. A parameter
// already modulated moves to the new bus in the slot it has, keeping its
// depth and mode - a route is edited, not replaced - and one that is not
// takes a free slot with the defaults `planModulation` uses, so the two ways
// of making a route cannot disagree about what a new one does.
export function planBusModulation(patch, caps, index, param, bus, options = {}) {
  if (!caps?.modRoutes) {
    return { ok: false, why: 'this firmware has no modulation routes' };
  }
  if (!Number.isInteger(bus) || bus < 0 || bus >= busCount(caps, Domain.CV)) {
    return { ok: false, why: 'that is not a CV bus' };
  }
  const target = patch.nodes[index];
  if (!target) return { ok: false, why: 'that node is no longer in the patch' };
  const pd = paramDescriptorOf(options.device, target.algorithmId, param);
  const name = pd?.name ?? `param ${param}`;

  const existing = routeTo(patch, index, param);
  if (existing) {
    const { slot: at, ...route } = existing;
    return { ok: true, domain: Domain.CV, bus, writes: [],
             routes: [{ slot: at, route: { ...route, buses: [bus] } }],
             said: `${name} now reads CV bus ${bus}` };
  }
  const slot = freeModSlot(patch, caps.modRoutes);
  if (slot === null) {
    return { ok: false, why: `every one of the module's ${caps.modRoutes} modulation routes is in use` };
  }
  const route = {
    buses: [bus], targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: index, param,
    min: 0, max: 0,
    depth: options.depth ?? 255,
    flags: options.flags ?? P.ModFlags.MOD_BIPOLAR | P.ModMode.MOD_OFFSET,
  };
  return { ok: true, domain: Domain.CV, bus, writes: [], routes: [{ slot, route }],
           said: `CV bus ${bus} → ${name}` };
}
