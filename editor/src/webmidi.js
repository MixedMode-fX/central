// The Web MIDI transport, and an honest account of where it works.
//
// Browser reach is a real constraint, not a footnote: this editor is the
// primary way to configure a module with no panel controls, so a browser
// without Web MIDI is not a degraded experience, it is a user who cannot set
// their module up. Hence `describeSupport()`, which the page shows before
// anything else, and the `.syx` export that works everywhere.

import * as P from './protocol.js';

// Verify this against current browser support before relying on it; the
// landscape moves. As of writing: Chrome, Edge and Opera support Web MIDI;
// Firefox supports it behind a permission prompt in recent versions; Safari
// does not. SysEx needs explicit permission everywhere, and Web MIDI needs a
// secure context - https:// or localhost, never file://.
export function describeSupport() {
  const secure = globalThis.isSecureContext === true;
  const available = typeof navigator !== 'undefined' && typeof navigator.requestMIDIAccess === 'function';
  if (!available) {
    return {
      ok: false,
      reason: 'This browser has no Web MIDI. Chrome, Edge and Opera have it; '
            + 'Firefox asks permission for it; Safari does not have it at all.',
      fallback: true,
    };
  }
  if (!secure) {
    return {
      ok: false,
      reason: 'Web MIDI needs a secure context. Open this page over https:// or '
            + 'from localhost - a file:// page cannot reach MIDI.',
      fallback: true,
    };
  }
  return { ok: true };
}

export async function requestAccess() {
  // sysex: true is the whole point; without it the browser hands back a MIDI
  // access that silently drops every message this editor sends.
  return navigator.requestMIDIAccess({ sysex: true });
}

// One MIDI input/output pair, as a transport the Device can use.
export class WebMidiTransport {
  constructor(input, output) {
    this.input = input;
    this.output = output;
    this.handler = () => {};
    input.onmidimessage = (event) => this.handler(event.data);
  }
  onMessage(fn) { this.handler = fn; }
  send(bytes) { this.output.send(Array.from(bytes)); }
  get name() { return this.output.name; }
}

// Finds the module by asking, rather than making a user pick a port by name.
//
// The identity request goes out on every output; the port whose *input*
// answers is the module's, and the output with the same name is the one to
// talk back on. The module blinks both LEDs when it answers, which is the only
// way to tell two of them apart.
export async function discover(access, { timeoutMs = 1500 } = {}) {
  const outputs = [...access.outputs.values()];
  const inputs = [...access.inputs.values()];
  const found = [];

  const request = [
    0xf0, P.SYSEX_UNIVERSAL_NON_REALTIME, P.SYSEX_BROADCAST_DEVICE,
    P.SYSEX_GENERAL_INFORMATION, P.SYSEX_IDENTITY_REQUEST, 0xf7,
  ];

  await new Promise((resolve) => {
    const previous = new Map();
    for (const input of inputs) {
      previous.set(input, input.onmidimessage);
      input.onmidimessage = (event) => {
        const d = event.data;
        if (d[0] !== 0xf0 || d[1] !== P.SYSEX_UNIVERSAL_NON_REALTIME) return;
        if (d[4] !== P.SYSEX_IDENTITY_REPLY || d[5] !== P.SYSEX_MANUFACTURER) return;
        // Pair it with the output whose name matches, falling back to the
        // first output, which is right for a single-port device.
        const output = outputs.find((o) => o.name === input.name) ?? outputs[0];
        if (output && !found.some((f) => f.input === input)) {
          found.push({ input, output, deviceId: d[2], name: input.name });
        }
      };
    }
    for (const output of outputs) {
      try { output.send(request); } catch { /* a port that will not take SysEx is not ours */ }
    }
    setTimeout(() => {
      for (const [input, handler] of previous) input.onmidimessage = handler ?? null;
      resolve();
    }, timeoutMs);
  });

  return found;
}
