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
import { Controller } from './controller.js';
import { Library, toBase64 } from './storage.js';
import { el, nodeCard, busUsers } from './views.js';
import { connectNewNode } from './graph.js';
import { routingPanel, globalsPanel, mappingPanel, controllerPanel } from './midi.js';
import { toPatchJsonText, fromPatchJson } from './patchjson.js';
import { libraryTab } from './library.js';
import { EXAMPLES } from './examples.js';
import { playTab, metersPanel, refreshLive } from './perform.js';

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
  { key: 'patch', label: 'patch' },
  { key: 'midi', label: 'MIDI' },
  { key: 'library', label: 'library' },
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
    // Which disclosure sections are open. The page is rebuilt wholesale on
    // every edit, so anything the DOM remembers by itself - a <details>, the
    // scroll position inside a lane - is lost unless the app remembers it.
    // A binding editor that folded shut the moment you set its CC number was
    // not usable.
    this.opened = new Set();
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
      this.pendingError = 'Without the module the app has nothing to read the algorithms from. '
                        + 'Connect a module over Web MIDI, or serve the page with mmmc.wasm beside it.';
    }
    this.render();
  }

  // Everything a connection means, once, for whichever transport it is: read
  // what the firmware has, then take its running patch. The app is the same
  // client either way - that is the point of the transport seam.
  async adopt(transport, label, deviceId = P.SYSEX_DEFAULT_DEVICE) {
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
    this.status = `connected to ${label}`;
  }

  // Back to (or on to) the module in the page.
  async useModule({ silent = false } = {}) {
    if (!this.module) return;
    if (!silent) this.stashWorking();
    this.module.start();
    await this.adopt(this.module, this.module.name);
    this.usingModule = true;
    this.current = { id: null, name: 'untitled', dirty: false, savedAt: 0 };
    this.savedImage = null;
    // Painted once per animation frame, off the module's own frame callback,
    // rather than on a timer of its own. A timer is the wrong clock for this:
    // the lights, the scope and the playheads are showing what the module did
    // *between two frames*, and the module is the thing that knows when a
    // frame's worth of passes has just run.
    this.stopLive?.();
    this.stopLive = this.module.onFrame(() => this.refreshLive());
    if (!silent) {
      this.status = 'editing the built-in module';
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
        this.status = 'no module answered. Check it is plugged in, then try again — '
                    + 'or build a patch here and export a .syx file.';
        this.render();
        return;
      }
      const kept = this.stashWorking();
      const port = found[0];
      this.module?.stop();
      this.usingModule = false;
      this.stopLive?.();
      this.stopLive = null;
      await this.adopt(new WebMidiTransport(port.input, port.output), port.name, port.deviceId);
      this.current = { id: null, name: `on ${port.name}`, dirty: true, savedAt: 0 };
      this.savedImage = null;
      if (kept) this.status += ` — what you were editing is kept as “${kept.name}”`;
    } catch (error) {
      this.status = `could not connect: ${error.message}`;
    }
    this.render();
  }

  async connectController() {
    try {
      await this.controller.connect();
      this.status = 'MIDI devices found — choose one to play the module from';
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
    this.sendWhole();
  }

  removeNode(index) {
    this.patch.nodes.splice(index, 1);
    // Removing a node renumbers the ones after it, so bindings that pointed
    // past it would point at the wrong node. Drop them rather than silently
    // rebinding somebody's knob to something else.
    this.patch.ccMap = this.patch.ccMap.map((m) => {
      if (!m || m.targetKind !== P.CcTargetKind.CC_TARGET_NODE) return m;
      if (m.targetIndex === index) return null;
      if (m.targetIndex > index) return { ...m, targetIndex: m.targetIndex - 1 };
      return m;
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
      this.status = 'the module is still running the previous patch — '
                  + 'fix what is listed above and it goes over automatically';
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

  // --- controller bindings -------------------------------------------------

  bindingFor(nodeIndex, param) {
    return this.patch.ccMap.find((m) => m && m.sourceMask
      && m.targetKind === P.CcTargetKind.CC_TARGET_NODE
      && m.targetIndex === nodeIndex && m.param === param) ?? null;
  }

  async learn(nodeIndex, param) {
    if (this.offline) {
      this.pendingError = 'learn needs a module — but a binding can be typed in by hand '
                        + 'under MIDI, with no controller present';
      this.render();
      return;
    }
    const slot = this.patch.ccMap.findIndex((m) => !m || !m.sourceMask);
    if (slot < 0) { this.pendingError = 'every binding slot is in use'; this.render(); return; }
    this.learnTarget = { nodeIndex, param, slot };
    this.status = this.usingModule && !this.controller?.inputId
      ? 'turn a controller to bind it — connect one under MIDI → external controller first'
      : 'turn a controller to bind it';
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
      this.sendWhole(`picked up where you left off — “${this.current.name}”`);
    } catch (error) {
      // A patch image from an older format version is not a crash: it is a
      // patch this build cannot read, and saying so beats an empty page.
      this.status = `could not reopen the last patch (${error.message}); starting fresh`;
      this.library.clearWorking();
    }
  }

  savePatch({ asNew = false } = {}) {
    const bytes = this.image();
    try {
      const entry = this.library.save({
        id: asNew ? null : this.current.id,
        name: asNew ? `${this.current.name} copy` : this.current.name,
        bytes,
        nodes: this.patch.nodes.length,
      });
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
      this.tab = this.editingTab = 'patch';
      this.sendWhole(`loaded ${file.name} — save it to keep it in this browser`);
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
        this.pendingError, ' (tap to dismiss)') : null,
      problems.length ? el('div', { class: 'problems' },
        el('h4', {}, this.diverged
          ? 'the module has not taken this patch yet'
          : 'the module would reject this patch'),
        el('ul', {}, problems.map((p) => el('li', {}, `${p.where}: ${p.message}`)))) : null,
      notes.length ? el('div', { class: 'notes' },
        el('h4', {}, 'worth a look'),
        el('ul', {}, notes.map((p) => el('li', {}, `${p.where}: ${p.message}`)))) : null,
      this.tab === 'patch' ? this.patchTab() : null,
      this.tab === 'play' ? playTab(this) : null,
      this.tab === 'midi' ? this.midiTab() : null,
      this.tab === 'library' ? libraryTab(this) : null);
  }

  patchTab() {
    return el('div', {},
      this.device?.capabilities ? this.capacities() : null,
      metersPanel(this),
      this.jacks(),
      el('div', { class: 'nodes' }, this.patch.nodes.map((_, i) => nodeCard(this, i))),
      this.patch.nodes.length ? null : el('p', { class: 'hint' },
        'No nodes yet. Add one below — it arrives connected to a bus, so the patch stays valid.'),
      this.addBar());
  }

  midiTab() {
    if (!this.device?.capabilities) {
      return el('p', { class: 'hint' },
        'The module is not answering yet, so there is nothing to route MIDI to.');
    }
    return el('div', {}, controllerPanel(this), mappingPanel(this), routingPanel(this), globalsPanel(this));
  }

  header() {
    const where = this.usingModule
      ? 'the module in this page'
      : (this.device ? this.device.transport.name : 'nothing');
    return el('header', { class: 'top' },
      el('div', { class: 'title' },
        el('h1', {}, 'MMMC'),
        el('span', { class: 'patch-name' }, this.current.name, this.current.dirty ? ' •' : '')),
      el('div', { class: 'top-buttons' },
        el('button', {
          class: this.tab === 'play' ? 'active' : '',
          'aria-pressed': this.tab === 'play' ? 'true' : 'false',
          onclick: () => this.togglePlay(),
        }, this.tab === 'play' ? 'back to editing' : 'play'),
        el('button', { onclick: () => this.connect() },
          this.usingModule ? 'connect a module' : 'reconnect')),
      el('p', { class: `status ${this.offline ? 'offline' : 'online'}${this.diverged ? ' warn' : ''}` },
        this.status, el('span', { class: 'hint' }, ` · editing ${where}`)));
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
      }, t.label)));
  }

  // What the module can actually hold, read from it rather than assumed: an
  // app that lets you build a patch the module will reject is worse than no
  // app.
  capacities() {
    const c = this.device.capabilities;
    return el('div', { class: 'caps' },
      `${this.patch.nodes.length}/${c.nodes} nodes · `,
      `${c.gateBuses} gate, ${c.noteBuses} note, ${c.cvBuses} CV buses · `,
      `${c.maxIn} in / ${c.maxOut} out per node · `,
      `${c.nParams} parameters · ${c.slots} preset slots of ${c.slotBytes} bytes`);
  }

  jacks() {
    const jacks = this.patch.gatePorts.map((port, i) => {
      const select = el('select', { onchange: (e) => {
        const [direction, bus] = e.target.value.split(':').map(Number);
        this.patch.gatePorts[i] = { direction, bus: Number.isNaN(bus) ? P.NO_BUS : bus };
        const value = this.patch.gatePorts[i];
        this.edit(() => this.device.setGatePort(i, value.direction, value.bus), 'jack');
        this.render();
      } });
      select.append(el('option', { value: '0:255' }, 'unused'));
      const buses = this.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
      for (const direction of [1, 2]) {
        for (let b = 0; b < buses; b++) {
          const option = el('option', { value: `${direction}:${b}` },
            direction === 1 ? `in → gate bus ${b}` : `out ← gate bus ${b}`);
          if (port.direction === direction && port.bus === b) option.selected = true;
          select.append(option);
        }
      }
      return el('label', { class: 'jack' }, el('span', {}, `jack ${i + 1}`), select);
    });
    return el('section', { class: 'panel' },
      el('h2', {}, 'jacks'),
      el('p', { class: 'hint' },
        '“in” means the jack drives a gate bus; “out” means a gate bus drives the jack. '
        + 'Play them under play, at the top of the page.'),
      el('div', { class: 'jacks' }, jacks));
  }

  addBar() {
    if (!this.device?.algorithms?.length) {
      return el('div', { class: 'hint' },
        'The algorithms come from the module, so there is no list here that can have drifted '
        + 'from the firmware.');
    }
    // The summary follows the selection, so what an algorithm is for is read
    // before it is added rather than after it has been patched in.
    const select = el('select', { id: 'algo-pick', class: 'grow',
                                  onchange: (e) => { this.addPick = e.target.value; this.render(); } });
    for (const d of this.device.algorithms) {
      if (!d) continue;
      const option = el('option', { value: String(d.id), title: d.summary ?? '' },
        `${d.name} — ${d.nIn} in, ${d.nOut} out`);
      if (String(d.id) === this.addPick) option.selected = true;
      select.append(option);
    }
    this.addPick ??= select.value;
    const chosen = this.device.byId.get(Number(this.addPick));
    return el('section', { class: 'panel add' },
      el('h2', {}, 'add a node'),
      el('div', { class: 'row' }, select,
        el('button', { onclick: () => this.addNode(Number(select.value)) }, 'add')),
      chosen?.summary ? el('p', { class: 'summary' }, chosen.summary) : null);
  }
}

// A patch called "my patch (2)" must not become a file called "my patch (2)"
// with a slash in it on somebody's system.
const fileName = (name) => (name || 'patch').replace(/[^\w .-]+/g, '-').slice(0, 60).trim() || 'patch';

const app = new App();
app.boot();
