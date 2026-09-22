// The module the page is talking to: the one built into it, or one on the
// end of a cable. Everything a connection means is here, once, for whichever
// transport it is - the app is the same client either way, which is the point
// of the transport seam.

import * as P from '../protocol/generated.js';
import { Device } from '../protocol/device.js';
import { LIVE_GLOBALS } from '../protocol/names.js';
import { Monitor } from '../runtime/monitor.js';

// How often the page asks the module what its modulation routes are doing. A
// meter drawn faster than this says nothing more, and over a DIN cable every
// question is a round trip the musical stream is waiting for.
const MOD_POLL_MS = 100;

export class Session extends EventTarget {
  constructor({ state, live, render }) {
    super();
    this.state = state;
    this.live = live;
    this.render = render;
    this.device = null;
    this.usingModule = false;      // is the built-in module the one being edited?
    this.offline = true;
    // What each modulation route is doing, as the module last reported it
    // (src/control/mod_matrix.h). Live state, keyed by slot: nothing here is
    // saved, exported or sent anywhere.
    this.modLive = new Map();
    // Where each macro is, as the module last reported it (control/macros.h).
    // A macro deliberately stores no position, so this is the only account of
    // one - and like modLive it is live state: nothing here is saved,
    // exported or sent anywhere.
    this.macroLive = new Map();
    // What the module has each global set to *now*, by the name the stored
    // settings give it (protocol/names.js, LIVE_GLOBALS). The patch holds
    // what a setting was saved as, and a Key node or a bound controller
    // moves the running one without touching it (src/midi/global_key.h), so
    // these are two different numbers and an indicator showing only the
    // first is showing the wrong one.
    this.globalsLive = new Map();
    // The running module, as it reports itself (runtime/monitor.js): what
    // everything live on the page is painted from, whichever transport the
    // module is on the end of.
    this.monitor = new Monitor();
    this.modPolling = false;
    this.monitorPolling = false;
    this.stopPolls = null;
  }

  get transportName() { return this.usingModule ? 'built-in' : (this.device?.transport.name ?? 'nothing'); }

  // Read what the firmware has, then take its running patch.
  async adopt(transport, { deviceId = P.SYSEX_DEFAULT_DEVICE, usingModule = false } = {}) {
    this.device = new Device(transport);
    this.device.deviceId = deviceId;
    this.monitor.reset();
    this.device.addEventListener('device-event', (e) => this.onDeviceEvent(e.detail));
    await this.device.readCapabilities();
    await this.device.readAlgorithms();
    for (const descriptor of this.device.algorithms) await this.device.readParams(descriptor.id);
    const dumped = await this.device.dump();
    this.state.patch = dumped.patch;
    this.state.globals = dumped.globals;
    this.state.diverged = false;
    this.usingModule = usingModule;
    this.offline = false;
    this.state.status = 'connected';
    this.startPolls();
  }

  // Paint what moves from what the module last reported: after a frame,
  // and after a rebuild of the page, whose new elements have not seen one.
  tickLive() {
    this.live.tick({ monitor: this.monitor, activity: this.monitor.takeActivity() });
  }

  // The module originates a few things on its own - a Program Change recall,
  // a CC learn, an error behind the red LED - and the app follows rather than
  // polling. What the page does about each is a listener's business.
  async onDeviceEvent({ event, detail }) {
    if (event === P.SysexEvent.SYSEX_EVENT_PROGRAM_CHANGE
        || event === P.SysexEvent.SYSEX_EVENT_PATCH_APPLIED) {
      const dumped = await this.device.dump();
      this.dispatchEvent(new CustomEvent('replaced', { detail: {
        ...dumped,
        said: event === P.SysexEvent.SYSEX_EVENT_PROGRAM_CHANGE
          ? `the module recalled preset ${detail}`
          : 'the module applied a pending patch',
      } }));
    } else if (event === P.SysexEvent.SYSEX_EVENT_CC_LEARNED) {
      const dumped = await this.device.dump();
      this.state.patch.ccMap = dumped.patch.ccMap;
      const bound = this.state.patch.ccMap[detail];
      this.state.status = bound
        ? `bound CC ${bound.cc} to slot ${detail}`
        : `bound a controller to slot ${detail}`;
      this.dispatchEvent(new CustomEvent('learned', { detail }));
    } else {
      this.state.error = `the module reported error ${P.SysexErrorName[detail] ?? detail}`;
    }
    this.render();
  }

  // --- what the module is doing ----------------------------------------------

  // Two questions, asked on the animation frame and never in a page that is
  // not being looked at - `requestAnimationFrame` does not fire there.
  //
  // The monitor's frame is asked for every animation frame, one at a time:
  // it is everything since the last ask, so a slow cable answers slower and
  // loses nothing, and asking twice at once would split one record in two.
  // What modulation is doing is asked less often: a meter drawn faster than
  // MOD_POLL_MS says nothing more.
  startPolls() {
    if (this.stopPolls || !globalThis.requestAnimationFrame) return;
    let running = true;
    let last = 0;
    const tick = (ts) => {
      if (!running) return;
      requestAnimationFrame(tick);
      this.pollMonitor();
      if (ts - last < MOD_POLL_MS) return;
      last = ts;
      this.pollModState();
    };
    requestAnimationFrame(tick);
    this.stopPolls = () => { running = false; };
  }

  // One frame of the monitor (runtime/monitor.js): the buses the page is
  // reading and the nodes with a playhead on screen, then everything live is
  // painted from what came back.
  async pollMonitor() {
    if (this.monitorPolling || !this.device) return;
    this.monitorPolling = true;
    try {
      const frame = await this.device.monitorFrame(this.monitor.noteMask, [...this.live.nodeSlots]);
      this.monitor.ingest(frame);
      this.tickLive();
    } catch {
      // A module that has gone away is the connection's problem, not the
      // lights': they stay where the last frame left them.
    } finally {
      this.monitorPolling = false;
    }
  }

  // A modulation route moves a parameter between passes, so the number in
  // the patch is the set point and not what the node is running. The module
  // knows both, and the only honest way to show it is to ask - and only
  // about the routes that have a meter on the page.

  // One round of questions, never two at once: over a DIN cable a round trip
  // is milliseconds, and a queue of overlapping polls would outlive whatever
  // it was asked about.
  async pollModState() {
    if (this.modPolling || !this.device || this.state.diverged) return;
    const slots = [...this.live.modSlots];
    const macros = [...this.live.macroSlots];
    const globals = [...this.live.globalNames];
    if (!slots.length) this.modLive.clear();
    if (!macros.length) this.macroLive.clear();
    if (!globals.length) this.globalsLive.clear();
    if (!slots.length && !macros.length && !globals.length) return;
    this.modPolling = true;
    try {
      for (const slot of slots) {
        const state = await this.device.getModState(slot);
        if (state) this.modLive.set(slot, state); else this.modLive.delete(slot);
      }
      for (const index of macros) {
        const state = await this.device.getMacroState(index);
        if (state) this.macroLive.set(index, state); else this.macroLive.delete(index);
      }
      for (const name of globals) {
        const target = LIVE_GLOBALS[name];
        if (!target) continue;
        const value = await this.device.getControl(target.kind, 0, target.param);
        if (value === null) this.globalsLive.delete(name); else this.globalsLive.set(name, value);
      }
    } catch {
      // A module that has gone away is the connection's problem, not this
      // view's: the meters go quiet and every other panel is unaffected.
      this.modLive.clear();
      this.macroLive.clear();
      this.globalsLive.clear();
    } finally {
      this.modPolling = false;
    }
  }
}
