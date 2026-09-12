// MMMC: one app for the module.
//
// The module has no encoder, no switches and no display (`src/hardware.h`), so
// this is not a companion to a panel - it is how a patch gets built, and it is
// a shipping deliverable.
//
// It used to be two pages. The editor built patches and sent them over Web
// MIDI or into a module embedded in the page; the emulator ran that same
// embedded module and let you hear it, but took its patch as a blob of JSON
// pasted into a box. Two pages, two mental models, and the loop that matters -
// change a parameter, hear what it did - crossed between them through the
// clipboard.
//
// They are one app now, because they were always one thing:
//
//   * **the module runs in the page, always.** It is the firmware compiled to
//     WebAssembly (`module.js`), and it is both the *transport* the editor
//     talks to over the real SysEx protocol and the *machine* whose jacks,
//     LEDs, sequencer positions and MIDI output the page can show. So an edit
//     is heard as it is made: there is nothing to load, because the thing
//     being edited is the thing that is running.
//   * **a patch is kept in the browser** (`storage.js`), because neither the
//     module's four EEPROM slots nor the built-in module's RAM is somewhere to
//     keep work.
//   * **a controller plugged into the computer plays it** (`controller.js`),
//     which is also what makes "learn" work with no module in the room.
//
// None of this teaches the app anything about the firmware it did not ask for.
// The algorithms, their ports, every parameter's range and the module's real
// capacities are read from the device; the validator that accepts a patch is
// the firmware's own, running in the page.

import * as P from './protocol.js';
import * as codec from './codec.js';
import { Device } from './device.js';
import { validate, advise, Domain } from './validate.js';
import { describeSupport, discover, requestAccess, WebMidiTransport } from './webmidi.js';
import { EmbeddedModule } from './module.js';
import { Listener } from './audio.js';
import { drumSources } from './drums.js';
import { Controller } from './controller.js';
import { Library, toBase64 } from './storage.js';
import { el, busUsers, iconButton } from './views.js';
import { icon } from './icons.js';
import {
  connectNewNode, BlockKind, patchBlocks, freeBus, writtenBus, waitingBus, applyWrite,
  modParamName, planPortFlip, applyPortFlip, planBusModulation,
} from './graph.js';
import { forgetNode } from './layout.js';
import { canvasPanel, canvasInspector, geometry, ENDPOINTS } from './canvas.js';
import { keyPanel } from './key.js';
import { routingPanel, globalsPanel, mappingPanel, modulationPanel,
         controllerPanel } from './midi.js';
import { toPatchJsonText, fromPatchJson } from './patchjson.js';
import { libraryTab } from './library.js';
import { EXAMPLES } from './examples.js';
import { playTab, metersPanel, refreshLive } from './perform.js';
import { schemaTab } from './schema.js';

// What the app is *for*, in the order the work happens: build the patch, route
// the MIDI around it, keep it somewhere.
//
// **play is not one of them.** It is the emulator - the module running, with
// its jacks, its scope, its keyboard and its ears - and it is a *place you go
// to*, alongside the module on the end of a cable, not a fourth thing to edit.
// So it sits with "connect a module" at the top of the page: the two buttons
// there are the two answers to "which module am I listening to?".
//
// And the library was called "patches", with a button in the top row that went
// to the tab of the same name sitting right below it. One name for one thing,
// in one place.
const TABS = [
  { key: 'patch', label: 'patch', icon: 'patch' },
  // The key is one scale, one root and one register for the whole patch, and
  // it is what every node is measured against - not a MIDI setting, and no
  // longer filed under one. See key.js.
  { key: 'key', label: 'key', icon: 'key' },
  { key: 'midi', label: 'MIDI', icon: 'midi' },
  { key: 'library', label: 'library', icon: 'library' },
  // Not a fourth thing to edit either: the schema tab is where the module says
  // what it can do, in a form something else can read. See schema.js.
  { key: 'schema', label: 'schema', icon: 'schema' },
];

const AUTOSAVE_MS = 400;

class App {
  constructor() {
    this.library = new Library();
    this.device = null;
    this.module = null;            // the firmware, running in this page
    this.listener = null;          // the audio standing in for what is downstream
    this.controller = null;        // a MIDI controller plugged into this computer
    this.usingModule = false;      // is the built-in module the one being edited?
    this.patch = codec.emptyPatch();
    this.globals = codec.emptyGlobals();
    this.status = 'starting the module…';
    this.pendingError = null;
    this.offline = true;
    this.learnTarget = null;
    this.tab = 'patch';
    // Where "play" came from, so leaving it goes back rather than guessing.
    this.editingTab = 'patch';
    this.scopeAll = false;
    // Traces put away for a moment, by pressing their chip in a legend: the
    // scope's rows and the rolls' sources. A preference about looking, so it
    // lives here and not in the patch.
    this.scopeHidden = new Set();
    this.rollHidden = new Set();
    // The note buses the piano roll is reading. A bus is only read when
    // something asks for it, and what the roll asks for is "every bus this
    // patch writes" - so the roll follows the patch rather than needing to be
    // told, and a bus nothing writes costs nothing.
    this.watchedBuses = new Set();
    // The patch being edited: its name, and where it came from in the library.
    this.current = { id: null, name: 'untitled', dirty: false, savedAt: 0 };
    this.savedImage = null;        // the image as last saved, for the dirty mark
    this.autosaveTimer = null;
    this.stopLive = null;          // unsubscribes the per-frame repaint
    this.renderScheduled = false;
    // What the on-screen keyboard sends.
    this.play = { port: P.MidiPort.mmMIDI_USB_0, channel: 1, velocity: 100, octave: 4, cc: 74, ccValue: 64 };
    // The module has not got this patch. An incremental edit addresses a node
    // *by index*, so once the two disagree about the graph's shape, every one
    // of them is a message about a node the module does not have - which is
    // where SYSEX_ERR_BAD_ARGUMENT came from. See edit().
    this.diverged = false;
    this.addPick = null;
    this.example = null;
    // Whether the prompt on the schema tab carries the patch on screen. On,
    // because "change this" is the usual ask and an empty page is the easy
    // one to get back to.
    this.schemaWithPatch = true;
    // The canvas: where it is looked at from, what is selected on it, and the
    // geometry of the last thing drawn - which the drag handlers read, since
    // they run between renders. It is the one way of looking at a patch: the
    // block that is selected gets its card below the picture, and that card
    // is the whole of the detail the list of cards used to spread down the
    // page.
    this.canvas = { view: { x: 0, y: 0, k: 1 }, selected: null, fit: true, geom: null };
    // Blocks somebody dragged somewhere, for the patch being edited. A patch
    // with none is laid out from its own shape; see layout.js for why a
    // position is never part of a patch.
    this.canvasPositions = {};
    // Which disclosure sections are open. The page is rebuilt wholesale on
    // every edit, so anything the DOM remembers by itself - a <details>, the
    // scroll position inside a lane - is lost unless the app remembers it.
    // A binding editor that folded shut the moment you set its CC number was
    // not usable. The details panel under the canvas starts open: it is where
    // the block just selected is edited, and folding it is the exception.
    this.opened = new Set(['details']);
    // Where each horizontal scroller - a step lane - had been scrolled to, by
    // the key `views.js` gave it. Same reason, and it bites hardest on a
    // phone: a 32-step lane is several screens wide, so without this a tap on
    // step 20 rebuilds the page, scrolls the pattern back to step 1, and puts
    // the step after the one just edited off the screen.
    this.scrolled = new Map();
  }

  // Keep the roll's note-bus watches in step with the patch. Called on every
  // render, which is every edit: a connection dragged onto a new bus is a new
  // bus to show, and one dragged off is one to stop reading.
  syncNoteBuses() {
    const wanted = new Set();
    if (this.module && this.usingModule && this.device?.capabilities) {
      for (let bus = 0; bus < this.device.capabilities.noteBuses; bus++) {
        if (busUsers(this, Domain.Note, bus).writers.length) wanted.add(bus);
      }
    }
    for (const bus of [...this.watchedBuses]) {
      if (wanted.has(bus)) continue;
      this.module?.unwatchNoteBus(bus);
      this.watchedBuses.delete(bus);
    }
    for (const bus of wanted) {
      if (this.watchedBuses.has(bus)) continue;
      this.module.watchNoteBus(bus);
      this.watchedBuses.add(bus);
    }
  }

  // Keep the drum voices in step with the patch, on every render for the same
  // reason as the buses above: a drum machine added, removed, dragged onto
  // another bus or moved onto channel 10 is a different instrument to point
  // the kit at.
  //
  // A drum machine is heard because it is *in the patch*, not because somebody
  // added a player for it - so this, rather than the listen panel, is what
  // makes it audible, and it is audible while the patch tab is the one on
  // screen. The mask is the other half of that: one patched to a MIDI output
  // sends every hit twice as far as this page is concerned, once on the bus it
  // writes and once on the cable, and two of them is a flam nobody
  // programmed.
  syncDrums() {
    if (!this.listener) return;
    const sources = this.module && this.usingModule && this.device
      ? drumSources(this.device, this.patch) : [];
    this.listener.setDrumSources(sources);
    const buses = new Set(sources.filter((s) => s.kind === 'note').map((s) => s.bus));
    let mask = 0;
    for (const port of this.patch.midiOut) {
      if (port.targetMask && buses.has(port.bus)) mask |= port.targetMask;
    }
    this.listener.setDrumOutMask(mask);
  }

  // Put every remembered scroller back where it was, before the frame is
  // painted: see `scrolled`.
  restoreScroll() {
    for (const box of document.querySelectorAll('[data-scroll]')) {
      const at = this.scrolled.get(box.dataset.scroll);
      if (at) box.scrollLeft = at;
    }
  }

  isOpen(key) { return this.opened.has(key); }
  setOpen(key, open) {
    if (open) this.opened.add(key); else this.opened.delete(key);
  }

  // --- starting up ---------------------------------------------------------

  // The module starts with the page. Nothing here asks the user to press
  // anything first: an app whose whole premise is "the thing you are editing is
  // running" cannot open with it stopped.
  async boot() {
    this.render();
    try {
      this.module = await EmbeddedModule.load();
      this.module.start();
      this.listener = new Listener(this.module);
      this.listener.restore(this.library.readListen());
      this.controller = new Controller(this.module);
      this.controller.onChange = () => this.render();
      await this.useModule({ silent: true });
      this.restoreWorking();
    } catch (error) {
      this.status = `the built-in module did not start: ${error.message}`;
      this.pendingError = 'no algorithms: connect a module, or serve mmmc.wasm beside the page';
    }
    this.render();
  }

  // Everything a connection means, once, for whichever transport it is: read
  // what the firmware has, then take its running patch. The app is the same
  // client either way - that is the point of the transport seam.
  async adopt(transport, deviceId = P.SYSEX_DEFAULT_DEVICE) {
    this.device = new Device(transport);
    this.device.deviceId = deviceId;
    this.device.addEventListener('device-event', (e) => this.onDeviceEvent(e.detail));
    await this.device.readCapabilities();
    await this.device.readAlgorithms();
    for (const descriptor of this.device.algorithms) await this.device.readParams(descriptor.id);
    const dumped = await this.device.dump();
    this.patch = dumped.patch;
    this.globals = dumped.globals;
    this.offline = false;
    this.diverged = false;
    this.status = 'connected';
  }

  // Back to (or on to) the module in the page.
  async useModule({ silent = false } = {}) {
    if (!this.module) return;
    if (!silent) this.stashWorking();
    this.module.start();
    await this.adopt(this.module);
    this.usingModule = true;
    this.current = { id: null, name: 'untitled', dirty: false, savedAt: 0 };
    this.savedImage = null;
    this.resetCanvas();
    // Painted once per animation frame, off the module's own frame callback,
    // rather than on a timer of its own. A timer is the wrong clock for this:
    // the lights, the scope and the playheads are showing what the module did
    // *between two frames*, and the module is the thing that knows when a
    // frame's worth of passes has just run.
    this.stopLive?.();
    this.stopLive = this.module.onFrame(() => this.refreshLive());
    if (!silent) {
      this.status = 'connected';
      this.render();
    }
  }

  // A module on the end of a cable. Its running patch replaces what is on
  // screen - it is the module's patch that is being edited from here on - so
  // anything unsaved is put in the library first rather than dropped.
  async connect() {
    const support = describeSupport();
    if (!support.ok) { this.status = support.reason; this.render(); return; }
    this.status = 'looking for a module…';
    this.render();
    try {
      const access = await requestAccess();
      const found = await discover(access);
      if (!found.length) {
        this.status = 'no module answered';
        this.render();
        return;
      }
      const kept = this.stashWorking();
      const port = found[0];
      this.module?.stop();
      this.usingModule = false;
      this.stopLive?.();
      this.stopLive = null;
      await this.adopt(new WebMidiTransport(port.input, port.output), port.deviceId);
      this.current = { id: null, name: `on ${port.name}`, dirty: true, savedAt: 0 };
      this.savedImage = null;
      this.resetCanvas();
      if (kept) this.status += ` — kept “${kept.name}”`;
    } catch (error) {
      this.status = `could not connect: ${error.message}`;
    }
    this.render();
  }

  async connectController() {
    try {
      await this.controller.connect();
      this.status = 'MIDI devices found';
    } catch (error) {
      this.status = `could not reach the MIDI devices: ${error.message}`;
    }
    this.render();
  }

  // The module originates a few things on its own - a Program Change recall, a
  // CC learn, an error behind the red LED - and the app follows rather than
  // polling.
  async onDeviceEvent({ event, detail }) {
    if (event === P.SysexEvent.SYSEX_EVENT_PROGRAM_CHANGE
        || event === P.SysexEvent.SYSEX_EVENT_PATCH_APPLIED) {
      const dumped = await this.device.dump();
      this.patch = dumped.patch;
      this.globals = dumped.globals;
      this.diverged = false;
      this.resetCanvas();
      this.status = event === P.SysexEvent.SYSEX_EVENT_PROGRAM_CHANGE
        ? `the module recalled preset ${detail}`
        : 'the module applied a pending patch';
    } else if (event === P.SysexEvent.SYSEX_EVENT_CC_LEARNED) {
      const dumped = await this.device.dump();
      this.patch.ccMap = dumped.patch.ccMap;
      const bound = this.patch.ccMap[detail];
      this.status = bound
        ? `bound CC ${bound.cc} to slot ${detail}`
        : `bound a controller to slot ${detail}`;
      this.learnTarget = null;
    } else {
      this.pendingError = `the module reported error ${P.SysexErrorName[detail] ?? detail}`;
    }
    this.render();
  }

  // --- edits ---------------------------------------------------------------

  // An edit goes to the module if one is attached, and is kept locally
  // regardless: a patch built with nothing plugged in has to be exportable.
  //
  // **While the module does not have this patch, an incremental edit is not
  // an edit.** "Set node 3's inlet" means nothing to a module whose patch has
  // two nodes, and the module rightly refuses it - an error about an argument,
  // for what is really the app addressing a graph that is not there. So a
  // divergence is repaired first: send the whole patch, then carry on
  // incrementally. This is the one place the app is allowed to be ahead of the
  // device, and it is temporary by construction.
  edit(action, what = 'that edit') {
    if (this.offline || !this.device) { this.render(); return; }
    if (this.diverged) { this.sendWhole(); return; }
    Promise.resolve()
      .then(action)
      .catch((error) => {
        this.pendingError = `${what}: ${error.message}`;
        this.diverged = true;
        this.render();
      });
  }

  addNode(algorithmId) {
    const caps = this.device?.capabilities;
    if (caps && this.patch.nodes.length >= caps.nodes) {
      this.pendingError = `this module holds ${caps.nodes} nodes`;
      this.render();
      return;
    }
    const d = this.device?.byId.get(algorithmId);
    const node = codec.emptyNode(algorithmId);
    const said = d ? connectNewNode(this.device, this.patch, node, d) : [];
    this.patch.nodes.push(node);
    this.status = said.length
      ? `added ${d.name} — ${said.join(', ')}`
      : `added ${d?.name ?? algorithmId}`;
    // A block that arrives selected is one whose parameters are already on
    // screen, which is what "add a sequencer" is usually the first half of.
    this.canvas.selected = { kind: 'block', id: `node:${this.patch.nodes.length - 1}` };
    this.sendWhole();
  }

  removeNode(index) {
    this.patch.nodes.splice(index, 1);
    // The nodes after it are renumbered, so where they were drawn has to move
    // with them or deleting one block rearranges the rest of the canvas.
    this.rememberLayout(forgetNode(this.canvasPositions, index));
    // A drum sequencer's kit and level are keyed by node index too, so they
    // move with it rather than becoming the settings for whatever ends up at
    // that index next.
    this.listener?.forgetDrumNode(index);
    // And so does whatever was selected: keeping the selection on "node 5"
    // through a deletion would leave a different node's parameters on screen
    // under the name of the one that was being edited.
    const selected = /^node:(\d+)$/.exec(this.canvas.selected?.id ?? '');
    if (selected) {
      const was = Number(selected[1]);
      this.canvas.selected = was === index ? null
        : { kind: 'block', id: `node:${was > index ? was - 1 : was}` };
    }
    // Removing a node renumbers the ones after it, so bindings that pointed
    // past it would point at the wrong node. Drop them rather than silently
    // rebinding somebody's knob to something else.
    this.patch.ccMap = this.patch.ccMap.map((m) => {
      if (!m || m.targetKind !== P.CcTargetKind.CC_TARGET_NODE) return m;
      if (m.targetIndex === index) return null;
      if (m.targetIndex > index) return { ...m, targetIndex: m.targetIndex - 1 };
      return m;
    });
    // Modulation routes name a node the same way, so they move the same way.
    this.patch.modMap = (this.patch.modMap ?? []).map((r) => {
      if (!r || r.targetKind !== P.CcTargetKind.CC_TARGET_NODE) return r;
      if (r.targetIndex === index) return null;
      if (r.targetIndex > index) return { ...r, targetIndex: r.targetIndex - 1 };
      return r;
    });
    this.sendWhole();
  }

  // Adding or removing a node changes the graph's shape, which is a whole
  // patch, not an incremental edit.
  //
  // A patch the app's own validator refuses is never sent: the module would
  // refuse it too, and the interesting part - *which* inlet, and why - is
  // already on screen. So the patch stays here, the app says the module is
  // still running the old one, and the next edit that makes it valid sends it.
  sendWhole(said = 'patch sent') {
    this.render();
    if (this.offline || !this.device) return;
    const problems = validate(this.device, this.patch);
    if (problems.length) {
      this.diverged = true;
      this.status = 'not sent: fix the problems above';
      this.render();
      return;
    }
    Promise.resolve()
      .then(() => this.device.sendPatch(this.patch, this.globals))
      .then(() => {
        this.diverged = false;
        this.status = said;
        // The nodes that were sounding are gone. A node handed over mid-note
        // publishes its note off outside a pass, where nothing is reading the
        // buses, so a player listening to one would drone on a note whose
        // owner no longer exists.
        this.listener?.panic();
      })
      .catch((error) => { this.diverged = true; this.pendingError = error.message; })
      .finally(() => this.render());
  }

  // --- the canvas ----------------------------------------------------------

  // Which patch the remembered block positions belong to. A patch nobody has
  // named yet is "working", the same identity the autosave uses, and it
  // follows the patch into the library the moment it is saved.
  layoutKey() { return this.current.id ?? 'working'; }

  rememberLayout(positions) {
    this.canvasPositions = positions;
    this.library.saveLayout(this.layoutKey(), positions);
  }

  // A block dragged somewhere. No render: the element is already there, and
  // rebuilding the page under a pointer that has only just been let go takes
  // the selection and the scroll with it.
  placeBlock(id, at) {
    this.rememberLayout({ ...this.canvasPositions,
                          [id]: [Math.round(at.x), Math.round(at.y)] });
  }

  select(selection) {
    this.canvas.selected = selection;
    this.render();
  }

  say(message) {
    this.status = message;
    this.render();
  }

  // The patch on screen was replaced by one nobody typed - loaded, imported,
  // dumped off a module. Take that patch's own arrangement, and put the view
  // back over the whole of it.
  resetCanvas() {
    this.canvas.selected = null;
    this.canvas.fit = true;
    this.canvasPositions = this.library.layoutFor(this.layoutKey()) ?? {};
  }

  // One port onto one bus - the same edit the inspector's selector makes, and
  // the same single message, whichever end of the patch the port is at. The
  // patch is changed by `applyWrite`, which the tests use too; what is left
  // here is which message says so.
  writePort(write) {
    const target = applyWrite(this.patch, write);
    if (!target) return;
    const { kind, index, port } = target;
    if (kind === 'mod') {
      this.edit(() => this.device.setModRoute(index, port), 'modulation route');
      return;
    }
    if (kind === BlockKind.Node) {
      this.edit(() => this.device.setConnection(index, Boolean(write.isOutlet), write.at, write.bus),
                'connection');
      return;
    }
    if (kind === BlockKind.Jack) {
      this.edit(() => this.device.setGatePort(index, port.direction, port.bus), 'jack');
      return;
    }
    const isOut = kind === BlockKind.MidiOut;
    const mask = isOut ? port.targetMask : port.sourceMask;
    this.edit(() => this.device.setMidiPort(index, isOut, mask, port.channel, port.bus),
              isOut ? 'MIDI out' : 'MIDI in');
  }

  // What a drag decided, applied. The decision is `graph.js`'s and is tested
  // against the firmware's validator; this is only the writing of it.
  applyPlan(plan) {
    if (!plan) return;
    if (!plan.ok) { this.status = plan.why; this.render(); return; }
    // Routes before ports, so a source claiming a bus on the way has the
    // route already pointed at the bus it is about to be put on: the module
    // validates a route against the *running* patch, and one naming a bus its
    // source has not reached yet is a route that does nothing until the next
    // edit.
    for (const { slot, route } of plan.routes ?? []) this.writeModRoute(slot, route);
    for (const write of plan.writes) this.writePort(write);
    this.status = plan.said;
    this.render();
  }

  // One modulation route, written to the patch and sent as the one message
  // that carries it. `null` clears the slot, which is how a route is removed:
  // a route with no bus is not a route (src/control/mod_matrix.h).
  writeModRoute(slot, route) {
    this.patch.modMap ??= Array.from({ length: P.N_MOD_ROUTE }, () => null);
    this.patch.modMap[slot] = route;
    this.edit(() => this.device.setModRoute(slot, route), 'modulation route');
  }

  // "add", for anything that can be on the canvas: an algorithm by its id, or
  // one of the edges of a patch by name.
  add(value) {
    const endpoint = ENDPOINTS.find((e) => e.key === value);
    if (endpoint) this.addEndpoint(endpoint);
    else this.addNode(Number(value));
  }

  // A jack or a MIDI port is not added so much as *taken into use*: the module
  // has a fixed eight of the first and four each of the second, so this claims
  // the first unused one and puts it on a bus - connected on arrival, for the
  // same reason a node is.
  addEndpoint(endpoint) {
    const caps = this.device?.capabilities;
    const blocks = patchBlocks(this.device, this.patch);
    const writesGate = endpoint.direction === P.GatePortDirection.GATE_PORT_IN;

    if (endpoint.kind === BlockKind.Jack) {
      const index = this.patch.gatePorts.findIndex(
        (port) => port.direction === P.GatePortDirection.GATE_PORT_UNUSED);
      if (index < 0) { this.pendingError = 'every jack is already in use'; this.render(); return; }
      const bus = writesGate
        ? (waitingBus(blocks, Domain.Gate) ?? freeBus(blocks, caps, Domain.Gate))
        : (writtenBus(blocks, Domain.Gate) ?? 0);
      if (bus === null) { this.pendingError = 'every gate bus is already written'; this.render(); return; }
      this.patch.gatePorts[index] = { direction: endpoint.direction, bus };
      this.canvas.selected = { kind: 'block', id: `jack:${index}` };
      this.edit(() => this.device.setGatePort(index, endpoint.direction, bus), 'jack');
      this.status = `jack ${index + 1} ${writesGate ? 'in' : 'out'}, on gate bus ${bus}`;
      this.render();
      return;
    }

    const isOut = endpoint.kind === BlockKind.MidiOut;
    const ports = isOut ? this.patch.midiOut : this.patch.midiIn;
    const limit = (isOut ? caps?.midiOut : caps?.midiIn) ?? ports.length;
    const index = ports.findIndex((port, i) => i < limit && !(isOut ? port.targetMask : port.sourceMask));
    if (index < 0) {
      this.pendingError = `every MIDI ${isOut ? 'output' : 'input'} port is already in use`;
      this.render();
      return;
    }
    const bus = isOut
      ? (writtenBus(blocks, Domain.Note) ?? 0)
      : (waitingBus(blocks, Domain.Note) ?? freeBus(blocks, caps, Domain.Note));
    if (bus === null) { this.pendingError = 'every note bus is already written'; this.render(); return; }
    const mask = P.MidiPort.mmMIDI_USB_0;
    ports[index] = { ...ports[index], bus, [isOut ? 'targetMask' : 'sourceMask']: mask };
    this.canvas.selected = { kind: 'block', id: `${endpoint.kind}:${index}` };
    this.edit(() => this.device.setMidiPort(index, isOut, mask, ports[index].channel, bus),
              isOut ? 'MIDI out' : 'MIDI in');
    this.status = `MIDI ${isOut ? 'out' : 'in'} ${index + 1} on USB 1, note bus ${bus}`;
    this.render();
  }

  // A MIDI port's direction, changed where its other settings are.
  //
  // Unlike a jack, this is not a field to write: the module has four inputs
  // and four outputs and they are different ports (`src/node/ports.h`), so
  // turning one round is *moving* it. The cables it is on, its channel and its
  // note bus travel to the first free port on the other side, and the one it
  // came from goes back to unused - so what the patch says afterwards is the
  // same port, pointing the other way, which is what a toggle promises.
  //
  // The bus travels too, and deliberately: a MIDI input on note bus 2 turned
  // round is a MIDI output *of* note bus 2, which is usually the monitoring
  // you were reaching for.
  flipMidiPort(index, isOut) {
    const plan = planPortFlip(this.patch, this.device?.capabilities, index, isOut);
    if (!plan.ok) { this.pendingError = plan.why; this.render(); return; }
    const left = (isOut ? this.patch.midiOut : this.patch.midiIn)[index];
    applyPortFlip(this.patch, plan);
    this.canvas.selected = {
      kind: 'block',
      id: `${plan.wantOut ? BlockKind.MidiOut : BlockKind.MidiIn}:${plan.to.index}`,
    };
    // Two messages, in this order: the port being left is silenced before the
    // one taking over speaks, so no moment of the flip has both of them on the
    // same note bus.
    this.edit(async () => {
      await this.device.setMidiPort(index, isOut, 0, left.channel, left.bus);
      await this.device.setMidiPort(plan.to.index, plan.wantOut, plan.mask, plan.channel, plan.bus);
    }, 'MIDI port direction');
    this.status = plan.said;
    this.render();
  }

  // Removing whatever kind of block it is. A node leaves the patch; a jack or
  // a MIDI port goes back to being unused, because the module has a fixed
  // number of them and they are not the patch's to delete.
  removeBlock(block) {
    if (block.kind === BlockKind.Node) { this.removeNode(block.index); return; }
    this.canvas.selected = null;
    if (block.kind === BlockKind.Jack) {
      this.patch.gatePorts[block.index] = { direction: P.GatePortDirection.GATE_PORT_UNUSED, bus: P.NO_BUS };
      this.edit(() => this.device.setGatePort(block.index, P.GatePortDirection.GATE_PORT_UNUSED, P.NO_BUS), 'jack');
      this.status = `jack ${block.index + 1} is unused`;
      this.render();
      return;
    }
    const isOut = block.kind === BlockKind.MidiOut;
    const port = (isOut ? this.patch.midiOut : this.patch.midiIn)[block.index];
    if (isOut) port.targetMask = 0; else port.sourceMask = 0;
    this.edit(() => this.device.setMidiPort(block.index, isOut, 0, port.channel, port.bus),
              isOut ? 'MIDI out' : 'MIDI in');
    this.status = `MIDI ${isOut ? 'out' : 'in'} ${block.index + 1} is unused`;
    this.render();
  }

  // --- controller bindings -------------------------------------------------

  bindingFor(nodeIndex, param) {
    return this.patch.ccMap.find((m) => m && m.sourceMask
      && m.targetKind === P.CcTargetKind.CC_TARGET_NODE
      && m.targetIndex === nodeIndex && m.param === param) ?? null;
  }

  // The modulation route reaching one parameter, with its slot, so the CV
  // button beside the control can say what it is on and clear it.
  routeFor(nodeIndex, param) {
    const slot = (this.patch.modMap ?? []).findIndex((r) => r && r.bus !== P.NO_BUS
      && r.targetKind === P.CcTargetKind.CC_TARGET_NODE
      && r.targetIndex === nodeIndex && r.param === param);
    return slot < 0 ? null : { slot, ...this.patch.modMap[slot] };
  }

  // A control signal onto a parameter, chosen from the parameter's side: the
  // CV button's bus menu. The plan is `graph.js`'s and the writing of it is
  // `applyPlan`'s, exactly as a drag on the canvas is.
  routeParam(nodeIndex, param, bus) {
    this.applyPlan(planBusModulation(this.patch, this.device?.capabilities, nodeIndex, param, bus,
                                     { device: this.device }));
  }

  async learn(nodeIndex, param) {
    if (this.offline) {
      this.pendingError = 'learn needs a module';
      this.render();
      return;
    }
    const slot = this.patch.ccMap.findIndex((m) => !m || !m.sourceMask);
    if (slot < 0) { this.pendingError = 'every binding slot is in use'; this.render(); return; }
    this.learnTarget = { nodeIndex, param, slot };
    this.status = 'turn a controller to bind it';
    this.render();
    this.edit(() => this.device.learnCc(slot, P.CcTargetKind.CC_TARGET_NODE, nodeIndex, param), 'learn');
  }

  learnInto(slot, mapping) {
    if (this.offline) { this.pendingError = 'learn needs a module'; this.render(); return; }
    this.learnTarget = { nodeIndex: mapping.targetIndex, param: mapping.param, slot };
    this.status = 'turn a controller to bind it';
    this.render();
    this.edit(() => this.device.learnCc(slot, mapping.targetKind, mapping.targetIndex, mapping.param), 'learn');
  }

  cancelLearn() {
    this.learnTarget = null;
    this.status = 'learn cancelled';
    this.edit(() => this.device.cancelLearn(), 'learn');
    this.render();
  }

  clearModRoute(slot) {
    const name = this.patch.modMap?.[slot]
      ? modParamName(this.device, this.patch, this.patch.modMap[slot]) : null;
    this.writeModRoute(slot, null);
    this.status = name ? `${name} is not modulated any more` : `modulation ${slot} is clear`;
    this.render();
  }

  clearMapping(slot) {
    const empty = { sourceMask: 0, channel: 0, cc: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE,
                    targetIndex: 0, param: 0, min: 0, max: 0, flags: 0 };
    this.patch.ccMap[slot] = null;
    this.edit(() => this.device.setCcMap(slot, empty), 'binding');
    this.render();
  }

  // --- the library ---------------------------------------------------------

  image() { return codec.encodePatch(this.patch, this.globals); }

  // Whatever is being edited, kept across a reload. This is not the library: a
  // patch nobody named is not something a user asked to keep a copy of, but
  // losing an afternoon's work to a tab reload is not acceptable either.
  autosave({ now = false } = {}) {
    clearTimeout(this.autosaveTimer);
    const write = () => {
      if (!this.library.available) return;
      const bytes = this.image();
      this.library.saveWorking({ id: this.current.id, name: this.current.name, bytes });
      const dirty = this.savedImage !== null && toBase64(bytes) !== this.savedImage;
      if (dirty !== this.current.dirty) {
        this.current.dirty = dirty;
        this.render();
      }
    };
    // Debounced, because a slider sweep is a hundred edits; immediate where
    // the next thing a user does might be closing the tab.
    if (now) write(); else this.autosaveTimer = setTimeout(write, AUTOSAVE_MS);
  }

  // The monitor setup, kept across a reload like the patch is. It is written
  // on the change itself rather than debounced: these are a handful of
  // deliberate choices, not a slider sweep.
  saveListen() {
    if (this.listener) this.library.saveListen(this.listener.toJSON());
  }

  restoreWorking() {
    const working = this.library.readWorking();
    if (!working) return;
    try {
      const { patch, globals } = codec.decodePatch(working.bytes);
      this.patch = patch;
      this.globals = globals;
      // Whether it counts as changed is decided against the library entry it
      // came from, not assumed: reopening a patch nobody touched must not
      // claim there is something to save.
      const entry = working.id ? this.library.get(working.id) : null;
      this.savedImage = entry ? toBase64(entry.bytes) : null;
      this.current = {
        id: entry ? entry.id : null,
        name: working.name ?? 'untitled',
        dirty: entry ? toBase64(working.bytes) !== this.savedImage : false,
        savedAt: entry?.updated ?? 0,
      };
      this.resetCanvas();
      this.sendWhole(`restored “${this.current.name}”`);
    } catch (error) {
      // A patch image from an older format version is not a crash: it is a
      // patch this build cannot read, and saying so beats an empty page.
      this.status = `could not reopen the last patch: ${error.message}`;
      this.library.clearWorking();
    }
  }

  savePatch({ asNew = false } = {}) {
    const bytes = this.image();
    const was = this.layoutKey();
    try {
      const entry = this.library.save({
        id: asNew ? null : this.current.id,
        name: asNew ? `${this.current.name} copy` : this.current.name,
        bytes,
        nodes: this.patch.nodes.length,
      });
      this.library.moveLayout(was, entry.id);
      this.current = { id: entry.id, name: entry.name, dirty: false, savedAt: entry.updated };
      this.savedImage = toBase64(bytes);
      this.status = `saved “${entry.name}”`;
      this.autosave({ now: true });
    } catch (error) {
      this.pendingError = error.message;
    }
    this.render();
  }

  loadSaved(id) {
    const entry = this.library.get(id);
    if (!entry) { this.pendingError = 'that patch is no longer in this browser'; this.render(); return; }
    try {
      const { patch, globals } = codec.decodePatch(entry.bytes);
      this.patch = patch;
      this.globals = globals;
      this.current = { id: entry.id, name: entry.name, dirty: false, savedAt: entry.updated };
      this.savedImage = toBase64(entry.bytes);
      this.resetCanvas();
      this.tab = this.editingTab = 'patch';
      this.autosave({ now: true });
      this.sendWhole(`loaded “${entry.name}”`);
    } catch (error) {
      this.pendingError = `could not read “${entry.name}”: ${error.message}`;
      this.render();
    }
  }

  // Deliberately without a render: this is called from a text field's own
  // change event, which fires when something else is clicked. Rebuilding the
  // list under that click would swallow it, and the field already shows the
  // new name.
  renameSaved(id, name) {
    const entry = this.library.rename(id, name.trim() || 'untitled');
    if (entry && entry.id === this.current.id) this.current.name = entry.name;
  }

  duplicateSaved(id) {
    this.library.duplicate(id);
    this.render();
  }

  deleteSaved(id) {
    this.library.remove(id);
    this.library.dropLayout(id);
    if (this.current.id === id) { this.current.id = null; this.savedImage = null; }
    this.status = 'deleted';
    this.render();
  }

  exportSaved(id) {
    const entry = this.library.get(id);
    if (!entry) return;
    this.downloadSyx(entry.bytes, `${fileName(entry.name)}.syx`);
  }

  // Anything unsaved, put somewhere it can be found again. Called before the
  // patch on screen is replaced by something the user did not type - the patch
  // running on a module they just plugged in.
  stashWorking() {
    if (!this.library.available || !this.patch.nodes.length) return null;
    if (this.current.id && !this.current.dirty) return null;
    try {
      return this.library.save({
        id: null,
        name: this.current.id ? `${this.current.name} (unsaved)` : this.current.name,
        bytes: this.image(),
        nodes: this.patch.nodes.length,
      });
    } catch {
      return null;
    }
  }

  // An example is loaded exactly as a file is: through the JSON dialect, into
  // the editor, unsaved. It is a starting point, not a preset the app owns.
  loadExample(name) {
    const example = EXAMPLES[name];
    if (!example) return;
    this.stashWorking();
    this.current = { id: null, name, dirty: true, savedAt: 0 };
    this.savedImage = null;
    this.loadJson(JSON.stringify(example.patch), `the “${name}” example`);
    this.tab = this.editingTab = 'patch';
  }

  newPatch() {
    this.stashWorking();
    this.patch = codec.emptyPatch();
    this.globals = codec.emptyGlobals();
    this.current = { id: null, name: 'untitled', dirty: false, savedAt: 0 };
    this.savedImage = null;
    this.resetCanvas();
    this.tab = this.editingTab = 'patch';
    this.autosave({ now: true });
    this.sendWhole('a new patch, empty');
  }

  // --- files ---------------------------------------------------------------

  download(name, text, type) {
    const blob = new Blob([text], { type });
    const link = el('a', { href: URL.createObjectURL(blob), download: name });
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  }

  // A .syx file is the fallback for a browser with no Web MIDI and the way a
  // patch is backed up: the same chunk messages the module receives, so any
  // standard SysEx librarian can send it.
  downloadSyx(image, name) {
    const chunks = codec.patchChunks(image, this.device?.deviceId ?? P.SYSEX_DEFAULT_DEVICE);
    const bytes = [];
    for (const chunk of chunks) bytes.push(...chunk);
    this.download(name, Uint8Array.from(bytes), 'application/octet-stream');
    this.status = `exported ${name}`;
    this.render();
  }

  exportSyx() { this.downloadSyx(this.image(), `${fileName(this.current.name)}.syx`); }

  exportJson() {
    this.download(`${fileName(this.current.name)}.json`, this.patchJson(), 'application/json');
    this.status = 'exported .json';
    this.render();
  }

  patchJson() {
    return toPatchJsonText(this.patch, this.globals, this.device);
  }

  async importFile(file) {
    try {
      if (/\.json$/i.test(file.name)) {
        this.loadJson(await file.text(), file.name, file.name.replace(/\.json$/i, ''));
        return;
      }
      const bytes = new Uint8Array(await file.arrayBuffer());
      // Either a raw image or the chunked SysEx a librarian would have saved.
      const image = bytes[0] === 0xf0 ? this.reassembleFile(bytes) : bytes;
      const { patch, globals } = codec.decodePatch(image);
      this.patch = patch;
      this.globals = globals;
      this.current = { id: null, name: file.name.replace(/\.[^.]+$/, ''), dirty: true, savedAt: 0 };
      this.savedImage = null;
      this.resetCanvas();
      this.tab = this.editingTab = 'patch';
      this.sendWhole(`loaded ${file.name}`);
    } catch (error) {
      this.pendingError = `could not read that file: ${error.message}`;
      this.render();
    }
  }

  loadJson(text, what = 'that JSON', name = null) {
    try {
      const { patch, globals } = fromPatchJson(JSON.parse(text), this.device);
      this.patch = patch;
      this.globals = globals;
      if (name) this.current = { id: null, name, dirty: true, savedAt: 0 };
      this.savedImage = null;
      this.resetCanvas();
      this.sendWhole(`loaded ${what}`);
    } catch (error) {
      this.pendingError = `could not read ${what}: ${error.message}`;
      this.render();
    }
  }

  reassembleFile(bytes) {
    const messages = [];
    let start = -1;
    for (let i = 0; i < bytes.length; i++) {
      if (bytes[i] === 0xf0) start = i;
      else if (bytes[i] === 0xf7 && start >= 0) { messages.push(bytes.subarray(start, i + 1)); start = -1; }
    }
    return codec.reassemble(messages.map((m) => {
      // The file holds host-to-device chunks; reassemble() reads the reply
      // shape, which differs only in the command byte.
      const copy = Uint8Array.from(m);
      if (copy[3] === P.SysexCommand.SYSEX_PATCH_CHUNK_IN) copy[3] = P.SysexCommand.SYSEX_PATCH_CHUNK_OUT;
      return copy;
    }));
  }

  // --- rendering -----------------------------------------------------------

  // Rebuilding the page is coalesced onto a microtask, and that is not an
  // optimisation.
  //
  // Replacing the tree removes whatever has focus, and removing a focused
  // input fires `change` *during* the replacement - which called render()
  // again, from inside the render that was already running. The outer
  // replaceChildren() then failed on a child list that had moved under it
  // ("the node to be removed is no longer a child of this node") and the
  // render was abandoned half done. So a render asked for while one is
  // running is deferred rather than run inside it.
  render() {
    if (this.renderScheduled) return;
    this.renderScheduled = true;
    queueMicrotask(() => {
      this.renderScheduled = false;
      this.syncNoteBuses();
      this.syncDrums();
      document.getElementById('app').replaceChildren(this.view());
      this.restoreScroll();
      this.autosave();
      this.refreshLive();
    });
  }

  // The LEDs, the jack lamps, the gate buses, the MIDI log and the sequencer
  // playheads, written straight into the DOM ten times a second. Re-rendering
  // the page for them would fight every open <select> and every held key.
  refreshLive() { refreshLive(this); }

  view() {
    const problems = this.device?.capabilities ? validate(this.device, this.patch) : [];
    const notes = this.device?.capabilities ? advise(this.device, this.patch) : [];

    return el('div', { class: 'shell' },
      this.header(),
      this.tabs(),
      this.pendingError ? el('div', { class: 'error', onclick: () => { this.pendingError = null; this.render(); } },
        this.pendingError) : null,
      problems.length ? el('div', { class: 'problems' },
        el('h4', {}, this.diverged ? 'not sent' : 'rejected'),
        el('ul', {}, problems.map((p) => el('li', {}, `${p.where}: ${p.message}`)))) : null,
      notes.length ? el('div', { class: 'notes' },
        el('h4', {}, 'notes'),
        el('ul', {}, notes.map((p) => el('li', {}, `${p.where}: ${p.message}`)))) : null,
      this.tab === 'patch' ? this.patchTab() : null,
      this.tab === 'play' ? playTab(this) : null,
      this.tab === 'key' ? this.keyTab() : null,
      this.tab === 'midi' ? this.midiTab() : null,
      this.tab === 'library' ? libraryTab(this) : null,
      this.tab === 'schema' ? schemaTab(this) : null);
  }

  // The patch, drawn. The canvas answers "what feeds what", which the bus
  // model otherwise hides in a dozen selectors reading "gate bus 2", and the
  // block that is selected gets the whole of its detail - every parameter, its
  // sequencer grid, its bus selectors, the roll of what it plays - in the
  // panel below the picture.
  patchTab() {
    if (!this.device?.capabilities) return el('p', { class: 'hint' }, 'no module');
    // Worked out once, before anything is built from it: the foot of the
    // canvas counts the free buses, and it has to be counting them in the
    // patch below it rather than in the one drawn before this edit.
    this.canvas.geom = geometry(this);
    return el('div', {},
      metersPanel(this),
      canvasPanel(this, this.canvas.geom),
      canvasInspector(this));
  }

  keyTab() {
    if (!this.device?.capabilities) return el('p', { class: 'hint' }, 'no module');
    return el('div', {}, keyPanel(this));
  }

  midiTab() {
    if (!this.device?.capabilities) {
      return el('p', { class: 'hint' }, 'no module');
    }
    return el('div', {}, controllerPanel(this), mappingPanel(this), modulationPanel(this),
                         routingPanel(this), globalsPanel(this));
  }

  header() {
    const where = this.usingModule
      ? 'built-in'
      : (this.device ? this.device.transport.name : 'nothing');
    return el('header', { class: 'top' },
      el('div', { class: 'title' },
        el('h1', {}, 'MMMC'),
        el('span', { class: 'patch-name' }, this.current.name, this.current.dirty ? ' •' : '')),
      el('div', { class: 'top-buttons' },
        iconButton({
          icon: this.tab === 'play' ? 'edit' : 'play',
          label: this.tab === 'play' ? 'back to editing' : 'play the module',
          text: this.tab === 'play' ? 'edit' : 'play',
          class: this.tab === 'play' ? 'active' : '',
          'aria-pressed': this.tab === 'play' ? 'true' : 'false',
          onclick: () => this.togglePlay(),
        }),
        iconButton({
          icon: 'plug', text: this.usingModule ? 'connect' : 'reconnect',
          label: this.usingModule ? 'connect a module over MIDI' : 'reconnect the module',
          onclick: () => this.connect(),
        })),
      el('p', { class: `status ${this.offline ? 'offline' : 'online'}${this.diverged ? ' warn' : ''}` },
        this.status, el('span', { class: 'hint' }, ` · ${where}`)));
  }

  // play is the module running, not a fourth thing to edit, so it is a button
  // at the top rather than a tab - and leaving it goes back to whichever tab it
  // was entered from.
  togglePlay() {
    if (this.tab === 'play') this.tab = this.editingTab;
    else { this.editingTab = this.tab; this.tab = 'play'; }
    this.render();
  }

  tabs() {
    return el('nav', { class: 'tabs', role: 'tablist' },
      TABS.map((t) => el('button', {
        class: `tab ${this.tab === t.key ? 'active' : ''}`,
        role: 'tab', 'aria-selected': this.tab === t.key ? 'true' : 'false',
        onclick: () => { this.tab = t.key; this.editingTab = t.key; this.render(); },
      }, icon(t.icon), el('span', { class: 'tab-label' }, t.label))));
  }

}

// A patch called "my patch (2)" must not become a file called "my patch (2)"
// with a slash in it on somebody's system.
const fileName = (name) => (name || 'patch').replace(/[^\w .-]+/g, '-').slice(0, 60).trim() || 'patch';

const app = new App();
app.boot();
