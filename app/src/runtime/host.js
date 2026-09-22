// The module the page is inside of.
//
// In the plugin (plugin/README.md) this page is the editor's window and the
// module is the plugin's own engine, which the host clocks and whose MIDI is
// the track's. The plugin puts `window.__JUCE__` in the page and names
// itself in its initialisation data, and that is how the page finds out: not
// by a build flag, so the one file emulator/build.sh produces is the page in
// a browser and the page in a plugin both.
//
// **It is a transport, like a cable.** `Device` talks to it over the real
// SysEx protocol, so every edit reaches the plugin as the message a cable
// would have carried and the validator is the firmware's own; a note from
// the keyboard or the surface goes over as the channel message a cable would
// have carried, on the cable the page chose (services/play.js). Nothing here
// is a side channel: the plugin's window sees exactly what its MIDI input
// would.
//
// The bridge is JUCE's: the page emits an event with a JSON payload and the
// plugin's listener for that name gets it; the plugin emits one and the
// page's listener gets it. Three names, all of them bytes:
//
//   mmmc.sysex     page -> plugin  one SysEx message, F0 to F7
//   mmmc.midi      page -> plugin  [cable, type, channel, d1, d2]
//   mmmc.message   plugin -> page  one SysEx message, F0 to F7

import * as P from '../protocol/generated.js';

const backendOf = () => globalThis.window?.__JUCE__?.backend ?? null;

// Whether this page is the plugin's window. The initialisation data is an
// array per key, so a plugin that named itself gives one entry: its build.
export function hosted() {
  const data = globalThis.window?.__JUCE__?.initialisationData?.mmmcHost;
  return Array.isArray(data) && data.length > 0 && backendOf() !== null;
}

export function hostBuild() {
  return hosted() ? String(globalThis.window.__JUCE__.initialisationData.mmmcHost[0]) : '';
}

export class HostTransport {
  constructor(backend = backendOf()) {
    if (!backend) throw new Error('this page is not inside the plugin');
    this.backend = backend;
    this.handler = () => {};
    backend.addEventListener('mmmc.message', (bytes) => {
      if (Array.isArray(bytes)) this.handler(Uint8Array.from(bytes));
    });
  }

  onMessage(fn) { this.handler = fn; }
  send(bytes) { this.backend.emitEvent('mmmc.sysex', Array.from(bytes)); }

  // A channel message on its way to the plugin's next pass, on the cable
  // the page chose - never the control cable, which carries the protocol.
  play(port, type, channel, d1, d2) {
    if (port & P.MIDI_CONTROL_PORT) return;
    this.backend.emitEvent('mmmc.midi', [port, type, channel, d1, d2]);
  }

  get name() { return 'the plugin'; }
  get deviceId() { return P.SYSEX_DEFAULT_DEVICE; }
}
