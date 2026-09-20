// Every edit to the patch, in one place: what changes in the state, which
// message says so to the module, and what the page says about it.
//
// The views never touch the device. A control writes a byte by asking the
// editor to, and the editor decides whether that is one message or a whole
// patch. The rules about the shape of a patch - what a drag means, which bus
// is free - are `core/graph.js`'s and are tested against the firmware's
// validator; this is only the writing of them.

import * as P from '../protocol/generated.js';
import * as codec from '../protocol/codec.js';
import { validate, Domain } from '../core/validate.js';
import {
  BlockKind, patchBlocks, unusedBus, applyWrite,
  planJackDirection, planPortFlip, applyPortFlip, planPortFanOut, planBusModulation, planCcBinding,
} from '../core/graph.js';
import {
  isBinding, isRoute, destsOfMacro, freeMacroDestSlot, renumberTargets, shiftNodeIndex, targetParamName,
} from '../core/patch.js';
import { ENDPOINTS } from '../core/catalogue.js';
import { selectBlock } from './state.js';

// A cleared binding, on the wire: the module has a fixed table, so a slot is
// emptied rather than removed.
const EMPTY_MAPPING = {
  sourceMask: 0, channel: 0, cc: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE,
  targetIndex: 0, param: 0, min: 0, max: 0, flags: 0,
};

const portsOf = (patch, isOut) => (isOut ? patch.midiOut : patch.midiIn);
const maskOf = (port, isOut) => (isOut ? port.targetMask : port.sourceMask);
const maskKey = (isOut) => (isOut ? 'targetMask' : 'sourceMask');
const portKind = (isOut) => (isOut ? BlockKind.MidiOut : BlockKind.MidiIn);
const portWhat = (isOut) => (isOut ? 'MIDI out' : 'MIDI in');

export class Editor {
  constructor({ state, session, arrangement, listener = null, render }) {
    this.state = state;
    this.session = session;
    this.arrangement = arrangement;
    this.listener = listener;
    this.render = render;
    // The parameter the module is waiting to bind a controller to, if any.
    this.learnTarget = null;
  }

  get device() { return this.session.device; }
  get patch() { return this.state.patch; }
  get caps() { return this.device?.capabilities ?? null; }

  say(message) { this.state.status = message; this.render(); }
  fail(message) { this.state.error = message; this.render(); }

  // --- sending ---------------------------------------------------------------

  // An edit goes to the module if one is attached, and is kept locally
  // regardless: a patch built with nothing plugged in has to be exportable.
  //
  // **While the module does not have this patch, an incremental edit is not
  // an edit.** "Set node 3's inlet" means nothing to a module whose patch has
  // two nodes. So a divergence is repaired first: send the whole patch, then
  // carry on incrementally. This is the one place the app is allowed to be
  // ahead of the device, and it is temporary by construction.
  edit(action, what = 'that edit') {
    if (this.session.offline || !this.device) { this.render(); return; }
    if (this.state.diverged) { this.sendWhole(); return; }
    Promise.resolve()
      .then(action)
      .catch((error) => {
        this.state.error = `${what}: ${error.message}`;
        this.state.diverged = true;
        this.render();
      });
  }

  // Adding or removing a node changes the graph's shape, which is a whole
  // patch. A patch the app's own validator refuses is never sent: the module
  // would refuse it too, and the interesting part is already on screen.
  sendWhole(said = 'patch sent') {
    this.render();
    if (this.session.offline || !this.device) return;
    if (validate(this.device, this.patch).length) {
      this.state.diverged = true;
      this.state.status = 'not sent: fix the problems above';
      this.render();
      return;
    }
    Promise.resolve()
      .then(() => this.device.sendPatch(this.patch, this.state.globals))
      .then(() => {
        this.state.diverged = false;
        this.state.status = said;
        // A node handed over mid-note publishes its note off outside a pass,
        // so a player listening to a bus would drone on a note whose owner
        // no longer exists.
        this.listener?.panic();
      })
      .catch((error) => { this.state.diverged = true; this.state.error = error.message; })
      .finally(() => this.render());
  }

  // --- one setting -----------------------------------------------------------

  setParam(index, at, value) {
    const clamped = Math.max(0, Math.min(255, value | 0));
    this.patch.nodes[index].params[at] = clamped;
    this.edit(() => this.device.setParam(index, at, clamped), 'parameter');
    this.render();
  }

  // Several bytes of one node, sent in order: a note step is a degree and a
  // velocity.
  setParams(index, writes) {
    const node = this.patch.nodes[index];
    for (const [at, value] of writes) node.params[at] = value & 0xff;
    this.edit(async () => {
      for (const [at] of writes) await this.device.setParam(index, at, node.params[at]);
    }, 'step');
    this.render();
  }

  setConnection(index, isOutlet, at, bus) {
    const node = this.patch.nodes[index];
    if (isOutlet) node.outBus[at] = bus; else node.inBus[at] = bus;
    this.edit(() => this.device.setConnection(index, isOutlet, at, bus), 'connection');
    this.render();
  }

  // A jack's direction and bus are one edit: `planJackDirection` says which
  // bus the jack ends up on, so "turn it round" and "move it" cannot grow two
  // rules that disagree.
  setJack(index, direction, bus) {
    const moved = {
      ...this.patch,
      gatePorts: this.patch.gatePorts.map((p, i) => (i === index ? { ...p, bus } : p)),
    };
    const chosen = planJackDirection(moved, this.caps, index, direction).bus;
    this.patch.gatePorts[index] = { direction, bus: chosen };
    this.edit(() => this.device.setGatePort(index, direction, chosen), 'jack');
    this.render();
  }

  // A MIDI port's cables, channel or bus, whichever of them changed.
  setMidiPort(index, isOut, changes) {
    const ports = portsOf(this.patch, isOut);
    ports[index] = { ...ports[index], ...changes };
    const port = ports[index];
    this.edit(() => this.device.setMidiPort(index, isOut, maskOf(port, isOut), port.channel, port.bus),
              portWhat(isOut));
    if (!maskOf(port, isOut)) this.state.status = `${portWhat(isOut)} ${index + 1} is unused`;
    this.render();
  }

  // The globals travel as one message; `what` is what the status says failed.
  setGlobals(changes, what = 'settings') {
    Object.assign(this.state.globals, changes);
    this.edit(() => this.device.setGlobals(this.state.globals), what);
    this.render();
  }

  // The clock's two cable masks, sent as the one message that carries them.
  setClockRoute(changes) {
    const g = Object.assign(this.state.globals, changes);
    this.edit(() => this.device.setClockRoute(g.clockInMask, g.clockOutMask), 'clock routing');
    this.render();
  }

  setNrpn(changes) {
    const g = Object.assign(this.state.globals, changes);
    this.edit(() => this.device.setNrpn(g.nrpnEnabled, g.nrpnChannel, g.nrpnSourceMask), 'NRPN');
    this.render();
  }

  // One modulation route, written and sent as the one message that carries
  // it. `null` clears the slot: a route with no bus is not a route.
  setModRoute(slot, route) {
    this.patch.modMap ??= Array.from({ length: P.N_MOD_ROUTE }, () => null);
    this.patch.modMap[slot] = route;
    this.edit(() => this.device.setModRoute(slot, route), 'modulation route');
    this.render();
  }

  // One controller binding, the same way. A row with no source port is not a
  // binding, which is how one is cleared.
  setCcMap(slot, mapping) {
    this.patch.ccMap[slot] = mapping;
    this.edit(() => this.device.setCcMap(slot, mapping ?? EMPTY_MAPPING), 'binding');
    this.render();
  }

  // --- macros ----------------------------------------------------------------
  // A macro is a target, not a source (src/control/macros.h): the app writes
  // its name and its destinations, and whatever moves it - a knob through the
  // binding table, a CV bus through the matrix - is bound like anything else.

  // The name, which is what makes a macro a macro. It is a fixed-width field
  // in the patch, so it is cut to length here rather than refused: a name
  // that does not fit is a name the module cannot hold.
  setMacro(index, name) {
    const limit = this.caps?.macroNameBytes ?? P.MACRO_NAME_BYTES;
    const text = String(name ?? '').slice(0, limit);
    this.patch.macros ??= Array.from({ length: P.N_MACRO }, () => null);
    this.patch.macros[index] = text ? { name: text } : null;
    this.edit(() => this.device.setMacro(index, text), 'macro name');
    this.render();
  }

  // One destination, in the pool slot it already occupies. `null` clears the
  // slot: a destination with no macro is not a destination.
  setMacroDest(slot, dest) {
    this.patch.macroDest ??= Array.from({ length: P.N_MACRO_DEST }, () => null);
    this.patch.macroDest[slot] = dest;
    this.edit(() => this.device.setMacroDest(slot, dest), 'macro destination');
    this.render();
  }

  clearMacroDest(slot) { this.setMacroDest(slot, null); }

  // A destination onto the first free pool entry. Both ways this can fail are
  // about capacity rather than about the destination, so both are said here
  // rather than arriving as a NAK: the pool is shared across every macro, and
  // one macro may not take more than its share of it.
  addMacroDest(dest) {
    const pool = this.caps?.macroDests ?? P.N_MACRO_DEST;
    const perMacro = this.caps?.macroDestsPerMacro ?? P.N_MACRO_DEST_PER_MACRO;
    if (destsOfMacro(this.patch, dest.macro).length >= perMacro) {
      this.fail(`a macro holds ${perMacro} destinations`);
      return null;
    }
    const slot = freeMacroDestSlot(this.patch, pool);
    if (slot === null) {
      this.fail(`all ${pool} macro destinations are in use`);
      return null;
    }
    this.setMacroDest(slot, dest);
    return slot;
  }

  // --- blocks ----------------------------------------------------------------

  // "add", for anything that can be on the canvas: an algorithm by its id, or
  // one of the edges of a patch by name.
  add(value) {
    const endpoint = ENDPOINTS.find((e) => e.key === value);
    if (endpoint) this.addEndpoint(endpoint);
    else this.addNode(Number(value));
  }

  addNode(algorithmId) {
    if (this.caps && this.patch.nodes.length >= this.caps.nodes) {
      this.fail(`this module holds ${this.caps.nodes} nodes`);
      return;
    }
    const d = this.device?.byId.get(algorithmId);
    // Unconnected, every port at NO_BUS: the wires are the ones you drag
    // (core/graph.js). A node whose required inlets are still empty makes the
    // patch incomplete, which the problems notice says and `sendWhole` acts
    // on, so the module is never handed a patch it would refuse.
    this.patch.nodes.push(codec.emptyNode(algorithmId));
    this.state.status = `added ${d?.name ?? algorithmId} — drag its inlets to connect it`;
    // A block that arrives selected is one whose parameters are already on
    // screen, which is what "add a sequencer" is usually the first half of.
    this.state.ui.canvas.selected = selectBlock(`node:${this.patch.nodes.length - 1}`);
    this.sendWhole();
  }

  removeNode(index) {
    this.patch.nodes.splice(index, 1);
    // Everything addressed by node index moves with the renumbering: the
    // positions, the drum voices, the selection, the bindings and the routes.
    this.arrangement.forgetNode(index);
    this.listener?.forgetDrumNode(index);
    const selected = /^node:(\d+)$/.exec(this.state.ui.canvas.selected?.id ?? '');
    if (selected) {
      const moved = shiftNodeIndex(Number(selected[1]), index);
      this.state.ui.canvas.selected = moved === null ? null : selectBlock(`node:${moved}`);
    }
    this.patch.ccMap = renumberTargets(this.patch.ccMap, index);
    this.patch.modMap = renumberTargets(this.patch.modMap ?? [], index);
    this.patch.macroDest = renumberTargets(this.patch.macroDest ?? [], index);
    this.sendWhole();
  }

  // A jack or a MIDI port is not added so much as *taken into use*: the
  // module has a fixed number of each, so this claims the first unused one.
  // It lands on a bus nothing else is on, because a jack always carries a bus
  // index and that is what unconnected looks like for one.
  addEndpoint(endpoint) {
    if (endpoint.kind !== BlockKind.Jack) { this.addMidiPort(endpoint.kind === BlockKind.MidiOut); return; }
    const blocks = patchBlocks(this.device, this.patch);
    const index = this.patch.gatePorts.findIndex(
      (port) => port.direction === P.GatePortDirection.GATE_PORT_UNUSED);
    if (index < 0) { this.fail('every jack is already in use'); return; }
    const writesGate = endpoint.direction === P.GatePortDirection.GATE_PORT_IN;
    const bus = unusedBus(blocks, this.caps, Domain.Gate);
    if (bus === null) { this.fail('every gate bus is already in use'); return; }
    this.patch.gatePorts[index] = { direction: endpoint.direction, bus };
    this.state.ui.canvas.selected = selectBlock(`jack:${index}`);
    this.edit(() => this.device.setGatePort(index, endpoint.direction, bus), 'jack');
    this.say(`jack ${index + 1} ${writesGate ? 'in' : 'out'}, on gate bus ${bus}`);
  }

  // One MIDI port, taken into use: the first free one, on USB 1, and on a note
  // bus nothing else is on - so it arrives connected to nothing, like a node.
  addMidiPort(isOut) {
    const blocks = patchBlocks(this.device, this.patch);
    const ports = portsOf(this.patch, isOut);
    const limit = (isOut ? this.caps?.midiOut : this.caps?.midiIn) ?? ports.length;
    const index = ports.findIndex((port, i) => i < limit && !maskOf(port, isOut));
    if (index < 0) { this.fail(`every MIDI ${isOut ? 'output' : 'input'} port is already in use`); return; }
    const bus = unusedBus(blocks, this.caps, Domain.Note);
    if (bus === null) { this.fail('every note bus is already in use'); return; }
    this.takeMidiPort(index, isOut, P.MidiPort.mmMIDI_USB_0, ports[index].channel, bus,
                      `${portWhat(isOut)} ${index + 1} on USB 1, note bus ${bus}`);
  }

  // The same source, reaching one more destination.
  fanOutMidiPort(index, isOut) {
    const plan = planPortFanOut(this.device, this.patch, this.caps, index, isOut);
    if (!plan.ok) { this.fail(plan.why); return; }
    this.takeMidiPort(plan.index, isOut, plan.mask, plan.channel, plan.bus, plan.said);
  }

  // A MIDI port written, sent and selected, which is what every way of
  // taking one into use ends in.
  takeMidiPort(index, isOut, mask, channel, bus, said) {
    const ports = portsOf(this.patch, isOut);
    ports[index] = { ...ports[index], channel, bus, [maskKey(isOut)]: mask };
    this.state.ui.canvas.selected = selectBlock(`${portKind(isOut)}:${index}`);
    this.edit(() => this.device.setMidiPort(index, isOut, mask, channel, bus), portWhat(isOut));
    this.say(said);
  }

  // Turning a MIDI port round is *moving* it (core/graph.js): what it carries
  // goes to the first free port on the other side. Two messages, in this
  // order: the port being left is silenced before the one taking over speaks.
  flipMidiPort(index, isOut) {
    const plan = planPortFlip(this.patch, this.caps, index, isOut);
    if (!plan.ok) { this.fail(plan.why); return; }
    const left = portsOf(this.patch, isOut)[index];
    applyPortFlip(this.patch, plan);
    this.state.ui.canvas.selected = selectBlock(`${portKind(plan.wantOut)}:${plan.to.index}`);
    this.edit(async () => {
      await this.device.setMidiPort(index, isOut, 0, left.channel, left.bus);
      await this.device.setMidiPort(plan.to.index, plan.wantOut, plan.mask, plan.channel, plan.bus);
    }, 'MIDI port direction');
    this.say(plan.said);
  }

  // Removing whatever kind of block it is. A node leaves the patch; a jack or
  // a MIDI port goes back to being unused.
  removeBlock(block) {
    if (block.kind === BlockKind.Node) { this.removeNode(block.index); return; }
    this.state.ui.canvas.selected = null;
    if (block.kind === BlockKind.Jack) {
      this.setJack(block.index, P.GatePortDirection.GATE_PORT_UNUSED, P.NO_BUS);
      this.say(`jack ${block.index + 1} is unused`);
      return;
    }
    this.setMidiPort(block.index, block.kind === BlockKind.MidiOut,
                     { [maskKey(block.kind === BlockKind.MidiOut)]: 0 });
  }

  // --- plans -----------------------------------------------------------------

  // One port onto one bus - the same edit the inspector's selector makes and
  // the same single message, whichever end of the patch the port is at.
  writePort(write) {
    const target = applyWrite(this.patch, write);
    if (!target) return;
    const { kind, index, port } = target;
    if (kind === 'mod') {
      this.edit(() => this.device.setModRoute(index, port), 'modulation route');
    } else if (kind === BlockKind.Node) {
      this.edit(() => this.device.setConnection(index, Boolean(write.isOutlet), write.at, write.bus),
                'connection');
    } else if (kind === BlockKind.Jack) {
      this.edit(() => this.device.setGatePort(index, port.direction, port.bus), 'jack');
    } else {
      const isOut = kind === BlockKind.MidiOut;
      this.edit(() => this.device.setMidiPort(index, isOut, maskOf(port, isOut), port.channel, port.bus),
                portWhat(isOut));
    }
  }

  // What a drag decided, applied. Routes before ports, so a source claiming a
  // bus on the way has the route already pointed at the bus it is about to
  // be put on: the module validates a route against the running patch.
  applyPlan(plan) {
    if (!plan) return;
    if (!plan.ok) { this.say(plan.why); return; }
    for (const { slot, route } of plan.routes ?? []) this.setModRoute(slot, route);
    for (const { slot, mapping } of plan.bindings ?? []) this.setCcMap(slot, mapping);
    for (const write of plan.writes) this.writePort(write);
    this.say(plan.said);
  }

  // A control signal onto a parameter, from the parameter's side. The plan
  // is `core/graph.js`'s, exactly as a drag on the canvas is.
  routeParam(index, param, bus) {
    this.applyPlan(planBusModulation(this.patch, this.caps, index, param, bus, { device: this.device }));
  }

  // A CC onto a parameter, named rather than turned: the binding a learn
  // would have made, with the number given instead of seen.
  bindParam(index, param, cc) {
    if (this.learnTarget) this.cancelLearn();
    this.applyPlan(planCcBinding(this.patch, this.caps, index, param, cc, { device: this.device }));
  }

  clearModRoute(slot) {
    const route = this.patch.modMap?.[slot];
    const name = isRoute(route) ? targetParamName(this.device, this.patch, route) : null;
    this.setModRoute(slot, null);
    this.say(name ? `${name} is not modulated any more` : `modulation ${slot} is clear`);
  }

  clearMapping(slot) {
    const mapping = this.patch.ccMap[slot];
    const name = isBinding(mapping) ? targetParamName(this.device, this.patch, mapping) : null;
    this.setCcMap(slot, null);
    this.say(name ? `${name} is not bound any more` : `binding ${slot} is clear`);
  }

  // --- learn -----------------------------------------------------------------

  learn(index, param) {
    this.learnInto({ targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: index, param });
  }

  // Arm the module to bind the next CC it sees to `target`, in `slot` - the
  // first free one unless a binding editor names its own.
  learnInto(target, slot = null) {
    if (this.session.offline) { this.fail('learn needs a module'); return; }
    const at = slot ?? this.patch.ccMap.findIndex((m) => !isBinding(m));
    if (at < 0) { this.fail('every binding slot is in use'); return; }
    this.learnTarget = { nodeIndex: target.targetIndex, param: target.param, slot: at };
    this.state.status = 'turn a controller to bind it';
    this.edit(() => this.device.learnCc(at, target.targetKind, target.targetIndex, target.param), 'learn');
    this.render();
  }

  cancelLearn() {
    this.learnTarget = null;
    this.state.status = 'learn cancelled';
    this.edit(() => this.device.cancelLearn(), 'learn');
    this.render();
  }

  learned() { this.learnTarget = null; }

  // Is the module waiting for a controller on this parameter? One learn is
  // armed at a time, and the page only remembers which control asked.
  isArmed(index, param) {
    const target = this.learnTarget;
    return Boolean(target && target.nodeIndex === index && target.param === param);
  }

  // --- the module's preset slots -------------------------------------------

  saveSlot(slot) {
    this.edit(async () => {
      await this.device.saveSlot(slot);
      this.say(`saved this patch to slot ${slot}`);
    }, 'save');
  }

  eraseSlot(slot) { this.edit(() => this.device.eraseSlot(slot), 'erase'); }
}
