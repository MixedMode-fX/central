// The Web MIDI transport, and an honest account of where it works.
//
// Browser reach is a real constraint, not a footnote: this app is the
// primary way to configure a module with no panel controls, so a browser
// without Web MIDI is not a degraded experience, it is a user who cannot set
// their module up. Hence `describeSupport()`, which the page shows before
// anything else, and the `.syx` export that works everywhere.

import * as P from '../protocol/generated.js';

// Verify this against current browser support before relying on it; the
// landscape moves. As of writing: Chrome, Edge and Opera support Web MIDI;
// Firefox supports it behind a permission prompt in recent versions; Safari
// does not. SysEx needs explicit permission everywhere, and Web MIDI needs a
// secure context - https:// or localhost, never file://.
export function describeSupport() {
  const secure = globalThis.isSecureContext === true;
  const available = typeof navigator !== 'undefined' && typeof navigator.requestMIDIAccess === 'function';
  if (!available) return { ok: false, reason: 'this browser has no Web MIDI' };
  if (!secure) return { ok: false, reason: 'Web MIDI needs https:// or localhost' };
  return { ok: true };
}

// One MIDI access for the whole app, asked for once.
//
// Two things want it - the control transport that talks to a module, and the
// external controller that plays the built-in one - and asking twice means a
// second permission prompt for something the user already allowed.
//
// `sysex: true` is what the control transport needs; without it the browser
// hands back an access that silently drops every message it sends. If that is
// refused we fall back to a plain access, because a controller playing the
// built-in module needs no SysEx at all and should not be lost to a permission
// that was only ever for hardware.
let pending = null;
export async function access() {
  pending ??= navigator.requestMIDIAccess({ sysex: true })
    .catch(() => navigator.requestMIDIAccess({ sysex: false }));
  try {
    return await pending;
  } catch (error) {
    pending = null;
    throw error;
  }
}

// Whether a port is still there. Unplugged, a port stays in the access with
// its state 'disconnected', and sending to one is not merely refused: Chrome
// on Windows can take the whole browser down with it. So nothing sends to a
// port this says is gone.
export const live = (port) => port?.state !== 'disconnected';

// "Teensy MIDI 2" and "Teensy MIDI" are two cables of one device, and so are
// Windows' "MIDIOUT2 (Teensy MIDI)" and "Teensy MIDI". A trailing number or a
// MIDIIN/MIDIOUT wrapper is how every host names them apart.
export const baseName = (name) => String(name ?? '')
  .replace(/^MIDI(?:IN|OUT)\d+\s*\((.*)\)$/, '$1')
  .replace(/\s*\d+$/, '').trim();

// One MIDI input/output pair, as a transport the Device can use.
export class WebMidiTransport {
  constructor(input, output) {
    this.input = input;
    this.output = output;
    this.handler = () => {};
    input.onmidimessage = (event) => this.handler(event.data);
  }
  onMessage(fn) { this.handler = fn; }
  send(bytes) {
    if (!live(this.output)) throw new Error(`${this.output.name} was unplugged`);
    this.output.send(Array.from(bytes));
  }
  // Deaf to the port from here on. Nothing is closed: a port that has gone
  // away is not one to call into.
  close() {
    this.handler = () => {};
    if (live(this.input)) this.input.onmidimessage = null;
  }
  get name() { return this.output.name; }
}

const isIdentityReply = (d) => d[0] === 0xf0 && d[1] === P.SYSEX_UNIVERSAL_NON_REALTIME
  && d[4] === P.SYSEX_IDENTITY_REPLY && d[5] === P.SYSEX_MANUFACTURER;

// Finds the module by asking, rather than making a user pick a port by name.
//
// The identity request goes out on one output at a time, and the input that
// answers it is that output's partner - whatever the host has named them.
// Asking every output at once was a race twice over: the replies came back in
// any order, so which cable won changed from one try to the next, and the
// module's USB stack has one SysEx buffer for all four cables, so requests
// arriving together on several could mangle each other into silence.
//
// Every port is opened, and waited for, before anything is sent. Setting
// `onmidimessage` opens an input too, but without waiting: a reply that
// arrived before the browser had finished opening it was dropped, and the
// next try - with the port now open - worked.
//
// The module blinks both LEDs when it answers, which is the only way to tell
// two of them apart.
export async function discover(access, { windowMs = 250 } = {}) {
  const outputs = [...access.outputs.values()].filter(live);
  const inputs = [...access.inputs.values()].filter(live);
  // A port another program holds is refused here, and is not ours anyway.
  await Promise.all([...inputs, ...outputs].map((port) => port.open().catch(() => {})));

  const request = [
    0xf0, P.SYSEX_UNIVERSAL_NON_REALTIME, P.SYSEX_BROADCAST_DEVICE,
    P.SYSEX_GENERAL_INFORMATION, P.SYSEX_IDENTITY_REQUEST, 0xf7,
  ];
  let heard = null;
  const previous = new Map();
  for (const input of inputs) {
    previous.set(input, input.onmidimessage);
    input.onmidimessage = (event) => { if (isIdentityReply(event.data)) heard?.(input, event.data); };
  }

  const found = [];
  try {
    for (const output of outputs) {
      const reply = await new Promise((resolve) => {
        const timer = setTimeout(() => resolve(null), windowMs);
        heard = (input, data) => { clearTimeout(timer); resolve({ input, data }); };
        try {
          output.send(request);
        } catch {
          // A port that will not take SysEx is not ours.
          clearTimeout(timer);
          resolve(null);
        }
      });
      heard = null;
      if (reply && !found.some((f) => f.input === reply.input)) {
        found.push({ input: reply.input, output, deviceId: reply.data[2], name: reply.input.name });
      }
    }
  } finally {
    for (const [input, handler] of previous) input.onmidimessage = handler ?? null;
  }
  return found;
}

// The cable to talk to the module on. It answers on all of them, but the
// protocol belongs on MIDI_CONTROL_PORT - the last of its cables, and a host
// lists a device's cables in order - so that a dump in flight and a pad being
// hit are never on the same one. Of two modules, the first found.
export function controlPort(found) {
  if (!found.length) return null;
  const base = baseName(found[0].output.name);
  return found.filter((f) => baseName(f.output.name) === base).at(-1);
}
