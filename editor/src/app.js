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
  }

  // An edit goes to the module if one is attached, and is kept locally
  // regardless: a patch built with nothing plugged in has to be exportable.
  edit(action) {
    if (this.offline || !this.device) { this.render(); return; }
    Promise.resolve()
      .then(action)
      .catch((error) => { this.pendingError = error.message; this.render(); });
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
      this.status = event === P.SysexEvent.SYSEX_EVENT_PROGRAM_CHANGE
        ? `the module recalled preset ${detail}`
        : 'the module applied a pending patch';
    } else if (event === P.SysexEvent.SYSEX_EVENT_CC_LEARNED) {
      const dumped = await this.device.dump();
      this.patch.ccMap = dumped.patch.ccMap;
      this.status = `bound a controller to slot ${detail}`;
      this.learnTarget = null;
    } else {
      this.pendingError = `the module reported error ${P.SysexErrorName[detail] ?? detail}`;
    }
    this.render();
  }

  addNode(algorithmId) {
    const caps = this.device?.capabilities;
    if (caps && this.patch.nodes.length >= caps.nodes) {
      this.pendingError = `this module holds ${caps.nodes} nodes`;
      this.render();
      return;
    }
    this.patch.nodes.push(codec.emptyNode(algorithmId));
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
  sendWhole() {
    this.edit(async () => {
      await this.device.sendPatch(this.patch, this.globals);
      this.status = 'patch sent';
    });
    this.render();
  }

  async learn(nodeIndex, param) {
    if (this.offline) { this.pendingError = 'learn needs a module'; this.render(); return; }
    const slot = this.patch.ccMap.findIndex((m) => !m || !m.sourceMask);
    if (slot < 0) { this.pendingError = 'every binding slot is in use'; this.render(); return; }
    this.learnTarget = { nodeIndex, param, slot };
    this.status = 'turn a controller to bind it';
    this.render();
    this.edit(() => this.device.learnCc(slot, P.CcTargetKind.CC_TARGET_NODE, nodeIndex, param));
  }

  exportSyx() {
    // A .syx file is the fallback for a browser with no Web MIDI and the way a
    // patch is backed up: the same chunk messages the module receives, so any
    // standard SysEx librarian can send it.
    const image = codec.encodePatch(this.patch, this.globals);
    const chunks = codec.patchChunks(image, this.device?.deviceId ?? P.SYSEX_DEFAULT_DEVICE);
    const bytes = [];
    for (const chunk of chunks) bytes.push(...chunk);
    const blob = new Blob([Uint8Array.from(bytes)], { type: 'application/octet-stream' });
    const link = el('a', { href: URL.createObjectURL(blob), download: 'mmmc-patch.syx' });
    document.body.append(link);
    link.click();
    link.remove();
  }

  async importSyx(file) {
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      // Either a raw image or the chunked SysEx a librarian would have saved.
      const image = bytes[0] === 0xf0 ? this.reassembleFile(bytes) : bytes;
      const { patch, globals } = codec.decodePatch(image);
      this.patch = patch;
      this.globals = globals;
      this.status = `loaded ${file.name}`;
      if (!this.offline) this.sendWhole();
    } catch (error) {
      this.pendingError = `could not read that file: ${error.message}`;
    }
    this.render();
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

  render() {
    const root = document.getElementById('app');
    root.replaceChildren(this.view());
  }

  view() {
    const problems = this.device?.capabilities ? validate(this.device, this.patch) : [];
    const notes = this.device?.capabilities ? advise(this.device, this.patch) : [];

    return el('div', { class: 'shell' },
      this.header(),
      this.pendingError ? el('div', { class: 'error', onclick: () => { this.pendingError = null; this.render(); } },
        this.pendingError, ' (click to dismiss)') : null,
      problems.length ? el('div', { class: 'problems' },
        el('h4', {}, 'the module would reject this patch'),
        el('ul', {}, problems.map((p) => el('li', {}, `${p.where}: ${p.message}`)))) : null,
      notes.length ? el('div', { class: 'notes' },
        el('h4', {}, 'worth a look'),
        el('ul', {}, notes.map((p) => el('li', {}, `${p.where}: ${p.message}`)))) : null,
      this.device?.capabilities ? this.capacities() : null,
      this.meters(),
      this.ports(),
      el('div', { class: 'nodes' }, this.patch.nodes.map((_, i) => nodeCard(this, i))),
      this.addBar(),
      this.presets());
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
      el('span', { class: `status ${this.offline ? 'offline' : 'online'}` }, this.status),
      el('button', { onclick: () => this.connect() }, this.offline ? 'connect' : 'reconnect'),
      el('button', { class: this.emulator ? 'active' : '', onclick: () => this.useEmulator() },
        this.emulator ? 'emulator running' : 'use built-in module'),
      el('button', { onclick: () => this.exportSyx() }, 'export .syx'),
      el('label', { class: 'file' }, 'import .syx',
        el('input', {
          type: 'file', accept: '.syx,.bin',
          onchange: (e) => { if (e.target.files[0]) this.importSyx(e.target.files[0]); },
        })));
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

  ports() {
    const jacks = this.patch.gatePorts.map((port, i) => {
      const select = el('select', { onchange: (e) => {
        const [direction, bus] = e.target.value.split(':').map(Number);
        this.patch.gatePorts[i] = { direction, bus: Number.isNaN(bus) ? P.NO_BUS : bus };
        const value = this.patch.gatePorts[i];
        this.edit(() => this.device.setGatePort(i, value.direction, value.bus));
        this.render();
      } });
      select.append(el('option', { value: '0:255' }, 'unused'));
      const buses = this.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
      for (const [direction, label] of [[1, 'in →'], [2, 'out ←']]) {
        for (let b = 0; b < buses; b++) {
          const option = el('option', { value: `${direction}:${b}` }, `${label} gate ${b}`);
          if (port.direction === direction && port.bus === b) option.selected = true;
          select.append(option);
        }
      }
      return el('label', { class: 'jack' }, el('span', {}, `jack ${i + 1}`), select);
    });
    return el('section', { class: 'panel' }, el('h2', {}, 'jacks'), el('div', { class: 'jacks' }, jacks));
  }

  addBar() {
    if (!this.device?.algorithms?.length) {
      return el('div', { class: 'hint' },
        'Connect a module to see the algorithms this firmware has. '
        + 'The editor reads them from the device, so it never has a list that has drifted.');
    }
    const select = el('select', { id: 'algo-pick' });
    for (const d of this.device.algorithms) {
      if (!d) continue;
      select.append(el('option', { value: String(d.id) },
        `${d.name} (${d.nIn} in, ${d.nOut} out)`));
    }
    return el('div', { class: 'add' }, select,
      el('button', { onclick: () => this.addNode(Number(select.value)) }, 'add node'));
  }

  presets() {
    if (this.offline) return null;
    const buttons = [];
    for (let s = 0; s < (this.device.capabilities?.slots ?? 0); s++) {
      buttons.push(el('div', { class: 'slot' },
        el('span', {}, `slot ${s}`),
        el('button', { onclick: () => this.edit(async () => {
          await this.device.saveSlot(s);
          this.status = `saved to slot ${s}`;
          this.render();
        }) }, 'save'),
        el('button', { onclick: () => this.edit(async () => {
          await this.device.loadSlot(s);
          const dumped = await this.device.dump();
          this.patch = dumped.patch;
          this.globals = dumped.globals;
          this.status = `recalled slot ${s}`;
          this.render();
        }) }, 'load'),
        el('button', { class: 'ghost', onclick: () => this.edit(() => this.device.eraseSlot(s)) }, 'erase')));
    }
    return el('section', { class: 'panel' },
      el('h2', {}, 'presets'),
      el('p', { class: 'hint' },
        'Program Change recalls these, once you turn recall on — it is off by default so a '
        + 'Program Change meant for a downstream synth cannot switch your patch.'),
      el('div', { class: 'slots' }, buttons));
  }
}

const app = new App();
const support = describeSupport();
app.status = support.ok
  ? 'not connected — press connect, or build a patch offline and export it'
  : support.reason;
app.render();
