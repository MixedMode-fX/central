// The editor.
//
// With no encoder, no switches and no display on the module, this is how a
// patch gets built. It is a client of the protocol in src/protocol/sysex.h and
// nothing more: everything it knows about what the firmware has, it asked the
// firmware for.

import * as P from './protocol.js';
import * as codec from './codec.js';
import { Device } from './device.js';
import { validate, advise } from './validate.js';
import { describeSupport, requestAccess, discover, WebMidiTransport } from './webmidi.js';
import { EmulatedModule } from './emulator.js';
import { el, nodeCard } from './views.js';
import { connectNewNode } from './graph.js';
import { routingPanel, globalsPanel, mappingPanel } from './midi.js';
import { toEmulatorText, fromEmulatorJson } from './emujson.js';

const TABS = [
  { key: 'patch', label: 'patch' },
  { key: 'midi', label: 'MIDI' },
  { key: 'presets', label: 'presets' },
  { key: 'share', label: 'files' },
];

class App {
  constructor() {
    this.device = null;
    this.patch = codec.emptyPatch();
    this.globals = codec.emptyGlobals();
    this.status = 'not connected';
    this.pendingError = null;
    this.offline = true;
    this.learnTarget = null;
    this.emulator = null;      // set when the embedded module is the transport
    this.meterTimer = null;
    this.tab = 'patch';
    // The module has not got this patch. An incremental edit addresses a node
    // *by index*, so once the two disagree about the graph's shape, every one
    // of them is a message about a node the module does not have - which is
    // where SYSEX_ERR_BAD_ARGUMENT came from. See edit().
    this.diverged = false;
    this.addPick = null;
    // Which disclosure sections are open. The page is rebuilt wholesale on
    // every edit, so anything the DOM remembers by itself - a <details>, the
    // scroll position inside a lane - is lost unless the app remembers it.
    // A binding editor that folded shut the moment you set its CC number was
    // not usable.
    this.opened = new Set();
  }

  isOpen(key) { return this.opened.has(key); }
  setOpen(key, open) {
    if (open) this.opened.add(key); else this.opened.delete(key);
  }

  // An edit goes to the module if one is attached, and is kept locally
  // regardless: a patch built with nothing plugged in has to be exportable.
  //
  // **While the module does not have this patch, an incremental edit is not
  // an edit.** "Set node 3's inlet" means nothing to a module whose patch has
  // two nodes, and the module rightly refuses it - an error about an argument,
  // for what is really the editor addressing a graph that is not there. So a
  // divergence is repaired first: send the whole patch, then carry on
  // incrementally. This is the one place the editor is allowed to be ahead of
  // the device, and it is temporary by construction.
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

  // Everything a connection means, once, for whichever transport it is: read
  // what the firmware has, then take its running patch. The editor is the
  // same client either way - that is the point of the transport seam.
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

  // The firmware, compiled to WebAssembly, running in this page. It is the
  // same code a module runs and it speaks the same protocol, so the editor
  // cannot tell the difference - and it needs no Web MIDI, which is what
  // makes the editor usable on a phone at all.
  async useEmulator() {
    this.status = 'starting the built-in module…';
    this.render();
    try {
      const emulator = await EmulatedModule.load();
      emulator.start();
      this.emulator = emulator;
      await this.adopt(emulator, emulator.name);
      // The two status LEDs and the live gate buses are the one thing a page
      // can show that a MIDI cable cannot; poll them off the render path.
      clearInterval(this.meterTimer);
      this.meterTimer = setInterval(() => this.renderMeters(), 100);
    } catch (error) {
      this.emulator = null;
      this.status = `could not start the built-in module: ${error.message}`;
    }
    this.render();
  }

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
                    + 'or build a patch offline and export a .syx file.';
        this.render();
        return;
      }
      const port = found[0];
      this.emulator?.stop();
      this.emulator = null;
      clearInterval(this.meterTimer);
      await this.adopt(new WebMidiTransport(port.input, port.output), port.name, port.deviceId);
    } catch (error) {
      this.status = `could not connect: ${error.message}`;
    }
    this.render();
  }

  // The module originates a few things on its own - a Program Change recall, a
  // CC learn, an error behind the red LED - and the editor follows rather than
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

  // --- the graph ----------------------------------------------------------

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
  // A patch the editor's own validator refuses is never sent: the module would
  // refuse it too, and the interesting part - *which* inlet, and why - is
  // already on screen. So the patch stays here, the editor says the module is
  // still running the old one, and the next edit that makes it valid sends it.
  sendWhole() {
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
      .then(() => { this.diverged = false; this.status = 'patch sent'; })
      .catch((error) => { this.diverged = true; this.pendingError = error.message; })
      .finally(() => this.render());
  }

  // --- controller bindings ------------------------------------------------

  bindingFor(nodeIndex, param) {
    return this.patch.ccMap.find((m) => m && m.sourceMask
      && m.targetKind === P.CcTargetKind.CC_TARGET_NODE
      && m.targetIndex === nodeIndex && m.param === param) ?? null;
  }

  async learn(nodeIndex, param) {
    if (this.offline) {
      this.pendingError = 'learn needs a module — but a binding can be typed in by hand '
                        + 'under MIDI control, with no controller present';
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

  clearMapping(slot) {
    const empty = { sourceMask: 0, channel: 0, cc: 0, targetKind: P.CcTargetKind.CC_TARGET_NODE,
                    targetIndex: 0, param: 0, min: 0, max: 0, flags: 0 };
    this.patch.ccMap[slot] = null;
    this.edit(() => this.device.setCcMap(slot, empty), 'binding');
    this.render();
  }

  // --- files --------------------------------------------------------------

  download(name, text, type) {
    const blob = new Blob([text], { type });
    const link = el('a', { href: URL.createObjectURL(blob), download: name });
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  }

  exportSyx() {
    // A .syx file is the fallback for a browser with no Web MIDI and the way a
    // patch is backed up: the same chunk messages the module receives, so any
    // standard SysEx librarian can send it.
    const image = codec.encodePatch(this.patch, this.globals);
    const chunks = codec.patchChunks(image, this.device?.deviceId ?? P.SYSEX_DEFAULT_DEVICE);
    const bytes = [];
    for (const chunk of chunks) bytes.push(...chunk);
    this.download('mmmc-patch.syx', Uint8Array.from(bytes), 'application/octet-stream');
    this.status = 'exported mmmc-patch.syx';
    this.render();
  }

  exportJson() {
    this.download('mmmc-patch.json', this.patchJson(), 'application/json');
    this.status = 'exported mmmc-patch.json';
    this.render();
  }

  patchJson() {
    return toEmulatorText(this.patch, this.globals, this.device);
  }

  async importFile(file) {
    try {
      if (/\.json$/i.test(file.name)) {
        this.loadJson(await file.text(), file.name);
        return;
      }
      const bytes = new Uint8Array(await file.arrayBuffer());
      // Either a raw image or the chunked SysEx a librarian would have saved.
      const image = bytes[0] === 0xf0 ? this.reassembleFile(bytes) : bytes;
      const { patch, globals } = codec.decodePatch(image);
      this.patch = patch;
      this.globals = globals;
      this.status = `loaded ${file.name}`;
      this.sendWhole();
    } catch (error) {
      this.pendingError = `could not read that file: ${error.message}`;
      this.render();
    }
  }

  loadJson(text, what = 'that JSON') {
    try {
      const { patch, globals } = fromEmulatorJson(JSON.parse(text), this.device);
      this.patch = patch;
      this.globals = globals;
      this.status = `loaded ${what}`;
      this.sendWhole();
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

  // --- rendering ----------------------------------------------------------

  render() {
    const root = document.getElementById('app');
    root.replaceChildren(this.view());
  }

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
      this.tab === 'midi' ? this.midiTab() : null,
      this.tab === 'presets' ? this.presets() : null,
      this.tab === 'share' ? this.shareTab() : null);
  }

  patchTab() {
    return el('div', {},
      this.device?.capabilities ? this.capacities() : null,
      this.meters(),
      this.jacks(),
      el('div', { class: 'nodes' }, this.patch.nodes.map((_, i) => nodeCard(this, i))),
      this.patch.nodes.length ? null : el('p', { class: 'hint' },
        'No nodes yet. Add one below — it arrives connected to a bus, so the patch stays valid.'),
      this.addBar());
  }

  midiTab() {
    if (!this.device?.capabilities) {
      return el('p', { class: 'hint' },
        'Connect a module — or press “use built-in module” — to route MIDI and bind controllers.');
    }
    return el('div', {}, mappingPanel(this), routingPanel(this), globalsPanel(this));
  }

  shareTab() {
    const json = this.patchJson();
    const box = el('textarea', { class: 'json', spellcheck: 'false', rows: '18',
                                 'aria-label': 'this patch as JSON' }, json);
    return el('div', {},
      el('section', { class: 'panel' },
        el('h2', {}, 'files'),
        el('p', { class: 'hint' },
          'A .syx file is the patch image — the bytes the module stores. Any SysEx librarian '
          + 'can send one, which is how a patch built with nothing plugged in reaches a module.'),
        el('div', { class: 'row' },
          el('button', { onclick: () => this.exportSyx() }, 'export .syx'),
          el('button', { onclick: () => this.exportJson() }, 'export .json'),
          el('label', { class: 'file' }, 'import a file',
            el('input', {
              type: 'file', accept: '.syx,.bin,.json',
              onchange: (e) => { if (e.target.files[0]) this.importFile(e.target.files[0]); },
            })))),
      el('section', { class: 'panel' },
        el('h2', {}, 'this patch as JSON'),
        el('p', { class: 'hint' },
          'The dialect the full emulator reads: paste this into its Patch JSON box and press '
          + 'Load to hear the patch. Algorithms are named rather than numbered, and bus indices '
          + 'are per domain. The emulator ignores the “globals” and “cc_map” keys, which are '
          + 'there so re-importing here loses nothing.'),
        box,
        el('div', { class: 'row' },
          el('button', { onclick: async () => {
            try {
              await navigator.clipboard.writeText(json);
              this.status = 'JSON copied to the clipboard';
            } catch {
              box.select();
              this.status = 'selected — copy it with your keyboard';
            }
            this.render();
          } }, 'copy'),
          el('button', { onclick: () => this.loadJson(box.value, 'the JSON above') }, 'load what is in the box'))));
  }

  // Updated on a timer rather than by re-rendering: the LEDs move at 10 Hz
  // and rebuilding the whole page for them would fight every open <select>.
  renderMeters() {
    if (!this.emulator) return;
    const leds = this.emulator.leds();
    const clock = this.emulator.clock();
    const green = document.getElementById('led-green');
    const red = document.getElementById('led-red');
    const beat = document.getElementById('clock-readout');
    if (green) green.style.opacity = String(Math.max(0.08, leds.green / 255));
    if (red) red.style.opacity = String(Math.max(0.08, leds.red / 255));
    if (beat) {
      beat.textContent = clock.running
        ? `${clock.bpm} BPM, beat ${Math.floor(clock.count / (P.MASTER_PPQN * 24)) + 1}`
        : 'clock stopped';
    }
    const buses = this.emulator.gateBuses();
    for (let b = 0; b < P.N_GATE_BUS; b++) {
      const dot = document.getElementById(`gate-${b}`);
      if (dot) dot.classList.toggle('lit', (buses & (1 << b)) !== 0);
    }
  }

  // With no panel feedback beyond two LEDs, a live view of what the module is
  // doing is its missing display - and in the page it costs nothing.
  meters() {
    if (!this.emulator) return null;
    const dots = [];
    for (let b = 0; b < P.N_GATE_BUS; b++) {
      dots.push(el('span', { class: 'gate-dot', id: `gate-${b}`, title: `gate bus ${b}` }));
    }
    return el('section', { class: 'panel meters' },
      el('h2', {}, 'what the module is doing'),
      el('div', { class: 'meter-row' },
        el('span', { class: 'led green', id: 'led-green', title: 'green: the clock' }),
        el('span', { class: 'led red', id: 'led-red', title: 'red: attention' }),
        el('span', { class: 'clock-readout', id: 'clock-readout' }, ''),
        el('div', { class: 'gate-dots' }, dots)),
      el('p', { class: 'hint' },
        'The firmware, running in this page. Presets live in RAM, so a reload '
        + 'loses them, and the jacks go nowhere — open the emulator to hear a patch.'));
  }

  header() {
    return el('header', { class: 'top' },
      el('h1', {}, 'MMMC patch editor'),
      el('div', { class: 'top-buttons' },
        el('button', { onclick: () => this.connect() }, this.offline ? 'connect' : 'reconnect'),
        el('button', { class: this.emulator ? 'active' : '', onclick: () => this.useEmulator() },
          this.emulator ? 'built-in module running' : 'use built-in module')),
      el('p', { class: `status ${this.offline ? 'offline' : 'online'}${this.diverged ? ' warn' : ''}` },
        this.status));
  }

  tabs() {
    return el('nav', { class: 'tabs', role: 'tablist' },
      TABS.map((t) => el('button', {
        class: `tab ${this.tab === t.key ? 'active' : ''}`,
        role: 'tab', 'aria-selected': this.tab === t.key ? 'true' : 'false',
        onclick: () => { this.tab = t.key; this.render(); },
      }, t.label)));
  }

  // What the module can actually hold, read from it rather than assumed: an
  // editor that lets you build a patch the module will reject is worse than no
  // editor.
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
        '“in” means the jack drives a gate bus; “out” means a gate bus drives the jack.'),
      el('div', { class: 'jacks' }, jacks));
  }

  addBar() {
    if (!this.device?.algorithms?.length) {
      return el('div', { class: 'hint' },
        'Connect a module to see the algorithms this firmware has. '
        + 'The editor reads them from the device, so it never has a list that has drifted.');
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

  presets() {
    if (this.offline) {
      return el('p', { class: 'hint' }, 'Preset slots live on the module. Connect one to use them.');
    }
    const buttons = [];
    for (let s = 0; s < (this.device.capabilities?.slots ?? 0); s++) {
      buttons.push(el('div', { class: 'slot' },
        el('span', {}, `slot ${s}`),
        el('button', { onclick: () => this.edit(async () => {
          await this.device.saveSlot(s);
          this.status = `saved to slot ${s}`;
          this.render();
        }, 'save') }, 'save'),
        el('button', { onclick: () => this.edit(async () => {
          await this.device.loadSlot(s);
          const dumped = await this.device.dump();
          this.patch = dumped.patch;
          this.globals = dumped.globals;
          this.diverged = false;
          this.status = `recalled slot ${s}`;
          this.render();
        }, 'load') }, 'load'),
        el('button', { class: 'ghost', onclick: () => this.edit(() => this.device.eraseSlot(s), 'erase') }, 'erase')));
    }
    return el('section', { class: 'panel' },
      el('h2', {}, 'presets'),
      el('p', { class: 'hint' },
        'Program Change recalls these once you turn recall on, under MIDI — it is off by default '
        + 'so a Program Change meant for a downstream synth cannot switch your patch.'),
      el('div', { class: 'slots' }, buttons));
  }
}

const app = new App();
const support = describeSupport();
app.status = support.ok
  ? 'not connected — press connect, or build a patch offline and export it'
  : support.reason;
app.render();
