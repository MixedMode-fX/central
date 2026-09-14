// The module the page is talking to: the one built into it, or one on the
// end of a cable. Everything a connection means is here, once, for whichever
// transport it is - the app is the same client either way, which is the point
// of the transport seam.

import * as P from '../protocol/generated.js';
import { Device } from '../protocol/device.js';

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
    this.modPolling = false;
    this.stopModPoll = null;
  }

  get transportName() { return this.usingModule ? 'built-in' : (this.device?.transport.name ?? 'nothing'); }

  // Read what the firmware has, then take its running patch.
  async adopt(transport, { deviceId = P.SYSEX_DEFAULT_DEVICE, usingModule = false } = {}) {
    this.device = new Device(transport);
    this.device.deviceId = deviceId;
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
    this.startModPoll();
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

  // --- what modulation is doing --------------------------------------------

  // A modulation route moves a parameter between passes, so the number in
  // the patch is the set point and not what the node is running. The module
  // knows both, and the only honest way to show it is to ask.
  //
  // Polled, and only while somebody is looking: the question is only asked
  // about routes that have a meter on the page, and a tab in the background
  // asks nothing at all - `requestAnimationFrame` does not fire there.
  startModPoll() {
    if (this.stopModPoll || !globalThis.requestAnimationFrame) return;
    let running = true;
    let last = 0;
    const tick = (ts) => {
      if (!running) return;
      requestAnimationFrame(tick);
      if (ts - last < MOD_POLL_MS) return;
      last = ts;
      this.pollModState();
    };
    requestAnimationFrame(tick);
    this.stopModPoll = () => { running = false; };
  }

  // One round of questions, never two at once: over a DIN cable a round trip
  // is milliseconds, and a queue of overlapping polls would outlive whatever
  // it was asked about.
  async pollModState() {
    if (this.modPolling || !this.device || this.state.diverged) return;
    const slots = [...this.live.modSlots];
    if (!slots.length) { this.modLive.clear(); return; }
    this.modPolling = true;
    try {
      for (const slot of slots) {
        const state = await this.device.getModState(slot);
        if (state) this.modLive.set(slot, state); else this.modLive.delete(slot);
      }
    } catch {
      // A module that has gone away is the connection's problem, not this
      // view's: the meters go quiet and every other panel is unaffected.
      this.modLive.clear();
    } finally {
      this.modPolling = false;
    }
  }
}
