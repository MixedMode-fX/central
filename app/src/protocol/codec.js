// The patch image and the SysEx framing, in JavaScript.
//
// This is the one place the app has to reimplement firmware code rather
// than read it from the device, because a patch built offline - with no module
// attached - still has to produce the exact bytes `src/patch/patch_codec.cpp`
// produces. The constants it works from are generated (protocol.js), and
// `app/test/protocol.test.mjs` drives the real firmware, compiled to
// WebAssembly, over this codec: what this file encodes, the firmware's own
// decoder has to accept, and what the firmware dumps, this file has to decode
// to the same patch. A divergence is a failing test, not a corrupted module.

import * as P from './generated.js';

// --- 7-in-8 packing -------------------------------------------------------
// SysEx data bytes must be <= 0x7F. For every group of up to seven bytes, one
// byte carrying their high bits, then the seven low-7-bit values.

export function pack(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 7) {
    const group = bytes.slice(i, i + 7);
    let high = 0;
    group.forEach((b, k) => { if (b & 0x80) high |= 1 << k; });
    out.push(high);
    for (const b of group) out.push(b & 0x7f);
  }
  return Uint8Array.from(out);
}

export function unpack(bytes) {
  const out = [];
  let i = 0;
  while (i < bytes.length) {
    const high = bytes[i++];
    if (high & 0x80) throw new Error('packed payload is not 7-bit');
    const n = Math.min(7, bytes.length - i);
    if (n === 0) break;
    for (let k = 0; k < n; k++) {
      const value = bytes[i + k];
      if (value & 0x80) throw new Error('packed payload is not 7-bit');
      out.push(value | ((high & (1 << k)) ? 0x80 : 0));
    }
    i += n;
  }
  return Uint8Array.from(out);
}

// The per-chunk checksum: a 7-bit XOR, so a chunk corrupted in flight is
// caught before it reaches the module's staging buffer.
export function checksum(bytes) {
  let sum = 0;
  for (const b of bytes) sum ^= b;
  return sum & 0x7f;
}

// --- CRC-16/CCITT-FALSE ---------------------------------------------------
export function crc16(bytes) {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc & 0xffff;
}

// --- The patch image ------------------------------------------------------
// Layout: see the comment at the top of src/patch/patch_codec.cpp. This must
// match it byte for byte.

const GLOBALS_BYTES = 32;

export function emptyGlobals() {
  return {
    clockSource: 0,
    cvPpqn: 4,
    bpm: P.CLOCK_DEFAULT_BPM,
    // The cables the clock arrives on and leaves by (src/patch/patch_codec.h).
    // Nothing for the input mask is every musical cable; nothing for the
    // output mask is nowhere.
    clockInMask: 0,
    clockOutMask: 0,
    pcEnabled: 0,
    pcChannel: 1,
    pcSourceMask: 0,
    pcQuantise: 0,
    nrpnEnabled: 0,
    nrpnChannel: 0,
    nrpnSourceMask: 0,
    // The key every algorithm plays in (src/midi/global_key.h). Chromatic on
    // C is "no key set", and the register is where a node that names no
    // octave of its own plays.
    scale: P.ScaleId.SCALE_CHROMATIC,
    root: 0,
    rootOctave: P.KEY_DEFAULT_OCTAVE,
  };
}

// --- a port's buses ---------------------------------------------------------
//
// Every port names the *set* of buses it is on (src/bus/domain.h), and the
// app holds that set as a sorted list of bus numbers: `[]` is a port
// connected to nothing, `[3]` one connection, `[0, 3]` a merge. The image and
// the wire carry it as a bit per bus, so these two are where the list becomes
// a number and back.

export const busMask = (buses) => (buses ?? []).reduce((mask, b) => mask | (1 << b), 0) & 0xffff;

export function busList(mask) {
  const buses = [];
  for (let b = 0; b < 16; b++) if (mask & (1 << b)) buses.push(b);
  return buses;
}

// The unused marker in a MacroDest's `macro` field (src/node/patch.h).
export const MACRO_NONE = 0xff;

// A macro destination's depth is signed: one macro opening a filter while
// closing a delay is the move macros exist for.
function signed16(v) { return v >= 0x8000 ? v - 0x10000 : v; }

// A macro's name, as both the patch image and the wire carry it: a fixed
// field of MACRO_NAME_BYTES, padded with zeros and not terminated when it is
// full. Written once here because the image and SYSEX_SET_MACRO must not
// disagree about what an eight-character name looks like.
export function nameBytes(name) {
  const text = String(name ?? '').slice(0, P.MACRO_NAME_BYTES);
  return Array.from({ length: P.MACRO_NAME_BYTES },
                    (_, i) => (i < text.length ? text.charCodeAt(i) & 0x7f : 0));
}

export function nameFrom(bytes, at) {
  let name = '';
  for (let i = 0; i < P.MACRO_NAME_BYTES; i++) {
    const byte = bytes[at + i];
    if (byte) name += String.fromCharCode(byte);
  }
  return name;
}

export function emptyPatch() {
  return {
    gatePorts: Array.from({ length: P.GPIO_N }, () => ({ direction: 0, buses: [] })),
    midiIn: Array.from({ length: P.N_MIDI_IN_NODES }, () => ({ sourceMask: 0, channel: 0, buses: [] })),
    midiOut: Array.from({ length: P.N_MIDI_OUT_NODES }, () => ({ targetMask: 0, channel: 0, buses: [] })),
    nodes: [],
    ccMap: Array.from({ length: P.N_CC_MAP }, () => null),
    // Modulation routes: a CV bus reaching a parameter
    // (src/control/mod_matrix.h). Null is an unused slot, exactly as ccMap.
    modMap: Array.from({ length: P.N_MOD_ROUTE }, () => null),
    // Macros: one performance control moving several parameters at once
    // (src/node/patch.h). A macro exists when it has a name; its position is
    // deliberately not here, because a macro does not store where it was.
    macros: Array.from({ length: P.N_MACRO }, () => null),
    // The destination pool, shared across every macro rather than fixed per
    // macro, so one sweeping control with six destinations and three with one
    // each all fit. Null is an unused slot, exactly as ccMap.
    macroDest: Array.from({ length: P.N_MACRO_DEST }, () => null),
  };
}

export function emptyNode(algorithmId) {
  return {
    algorithmId,
    inBuses: Array.from({ length: P.MAX_IN }, () => []),
    outBuses: Array.from({ length: P.MAX_OUT }, () => []),
    params: new Uint8Array(P.N_PARAM),
  };
}

class Writer {
  constructor() { this.bytes = []; }
  u8(v) { this.bytes.push(v & 0xff); }
  u16(v) { this.u8(v & 0xff); this.u8((v >> 8) & 0xff); }
  u32(v) { this.u16(v & 0xffff); this.u16((v >>> 16) & 0xffff); }
  many(values) { for (const v of values) this.u8(v); }
  // A port's set of buses, a bit per bus.
  set(buses) { this.u16(busMask(buses)); }
  sets(lists) { for (const buses of lists) this.set(buses); }
}

class Reader {
  constructor(bytes) { this.bytes = bytes; this.at = 0; }
  u8() {
    if (this.at >= this.bytes.length) throw new Error('image ended mid-record');
    return this.bytes[this.at++];
  }
  u16() { const lo = this.u8(); return lo | (this.u8() << 8); }
  u32() { const lo = this.u16(); return (lo | (this.u16() << 16)) >>> 0; }
  many(n) { const out = []; for (let i = 0; i < n; i++) out.push(this.u8()); return out; }
  set() { return busList(this.u16()); }
  sets(n) { const out = []; for (let i = 0; i < n; i++) out.push(this.set()); return out; }
}

function writeGlobals(w, g) {
  // sizeof(GlobalSettings) bytes, in declaration order, then the reserved tail.
  w.u8(g.clockSource);
  w.u8(g.cvPpqn);
  w.u16(g.bpm);
  w.u8(g.clockInMask ?? 0);
  w.u8(g.clockOutMask ?? 0);
  w.u8(g.pcEnabled);
  w.u8(g.pcChannel);
  w.u8(g.pcSourceMask);
  w.u8(g.pcQuantise);
  w.u8(g.nrpnEnabled);
  w.u8(g.nrpnChannel);
  w.u8(g.nrpnSourceMask);
  w.u8(g.scale ?? P.ScaleId.SCALE_CHROMATIC);
  w.u8(g.root ?? 0);
  w.u8(g.rootOctave ?? 0);
  for (let i = 0; i < GLOBALS_BYTES - 16; i++) w.u8(0);
}

function readGlobals(r) {
  const g = {
    clockSource: r.u8(),
    cvPpqn: r.u8(),
    bpm: r.u16(),
    clockInMask: r.u8(),
    clockOutMask: r.u8(),
    pcEnabled: r.u8(),
    pcChannel: r.u8(),
    pcSourceMask: r.u8(),
    pcQuantise: r.u8(),
    nrpnEnabled: r.u8(),
    nrpnChannel: r.u8(),
    nrpnSourceMask: r.u8(),
    // A zero here is what an image written before the key existed carries,
    // and what the firmware reads as chromatic - so it is shown as chromatic
    // rather than as a scale the module cannot be in (src/midi/scale.h).
    scale: r.u8() || P.ScaleId.SCALE_CHROMATIC,
    root: r.u8(),
    // The register the key sits in. Zero names none, and reads as the
    // module's default (src/midi/global_key.h).
    rootOctave: r.u8(),
  };
  for (let i = 0; i < GLOBALS_BYTES - 16; i++) r.u8();
  return g;
}

// The last non-zero parameter plus one: an algorithm uses the first few bytes
// and leaves the rest zero, and the image does not carry the tail. This is
// what makes a patch fit a 1 KB preset slot when sizeof(Patch) is 11 KB.
function usedPorts(sets) {
  let n = sets.length;
  while (n > 0 && !(sets[n - 1] ?? []).length) n--;
  return sets.slice(0, n);
}

function usedParams(params) {
  let n = params.length;
  while (n > 0 && params[n - 1] === 0) n--;
  return n;
}

export function encodePatch(patch, globals = emptyGlobals()) {
  const w = new Writer();
  w.u32(P.PATCH_MAGIC);
  w.u8(P.PATCH_FORMAT_VERSION);
  w.u8(0);                       // flags
  w.u16(0);                      // payload length, filled in below
  const payloadStart = w.bytes.length;

  writeGlobals(w, globals);
  for (const port of patch.gatePorts) { w.u8(port.direction); w.set(port.buses); }
  for (const port of patch.midiIn) { w.u8(port.sourceMask); w.u8(port.channel); w.set(port.buses); }
  for (const port of patch.midiOut) { w.u8(port.targetMask); w.u8(port.channel); w.set(port.buses); }

  w.u8(patch.nodes.length);
  for (const node of patch.nodes) {
    w.u8(node.algorithmId);
    // Trailing unconnected ports are not carried, the same trimming the
    // parameters get: most algorithms use one or two inlets and one outlet.
    const ins = usedPorts(node.inBuses);
    w.u8(ins.length);
    w.sets(ins);
    const outs = usedPorts(node.outBuses);
    w.u8(outs.length);
    w.sets(outs);
    const n = usedParams(node.params);
    w.u16(n);
    for (let i = 0; i < n; i++) w.u8(node.params[i]);
  }

  const used = patch.ccMap.map((m, slot) => ({ m, slot })).filter(({ m }) => m && m.sourceMask);
  w.u8(used.length);
  for (const { m, slot } of used) {
    w.u8(slot);
    w.u8(m.sourceMask);
    w.u8(m.channel);
    w.u8(m.cc);
    w.u8(m.targetKind);
    w.u8(m.targetIndex);
    w.u16(m.param);
    w.u16(m.min);
    w.u16(m.max);
    w.u8(m.flags);
  }

  const routes = (patch.modMap ?? []).map((m, slot) => ({ m, slot }))
    .filter(({ m }) => m && (m.buses ?? []).length);
  w.u8(routes.length);
  for (const { m, slot } of routes) {
    w.u8(slot);
    w.set(m.buses);
    w.u8(m.targetKind);
    w.u8(m.targetIndex);
    w.u16(m.param);
    w.u16(m.min);
    w.u16(m.max);
    w.u8(m.depth);
    w.u8(m.flags);
  }

  // The name is a fixed-width field, not length-prefixed: this has to agree
  // byte for byte with src/patch/patch_codec.cpp, which is hand-rolled, and a
  // fixed field is the one shape the two cannot disagree about.
  const macros = (patch.macros ?? []).map((m, slot) => ({ m, slot }))
    .filter(({ m }) => m && m.name);
  w.u8(macros.length);
  for (const { m, slot } of macros) {
    w.u8(slot);
    w.many(nameBytes(m.name));
  }

  const dests = (patch.macroDest ?? []).map((d, slot) => ({ d, slot }))
    .filter(({ d }) => d && d.macro !== null && d.macro !== undefined && d.macro !== MACRO_NONE);
  w.u8(dests.length);
  for (const { d, slot } of dests) {
    w.u8(slot);
    w.u8(d.macro);
    w.u8(d.targetKind);
    w.u8(d.targetIndex);
    w.u16(d.param);
    w.u8(d.srcLo);
    w.u8(d.srcHi);
    w.u16(d.depth & 0xffff);        // two's complement; signed again on the way back
    w.u8(d.flags ?? 0);
  }

  const payload = w.bytes.length - payloadStart;
  w.bytes[6] = payload & 0xff;
  w.bytes[7] = (payload >> 8) & 0xff;
  const crc = crc16(w.bytes);
  w.u16(crc);
  return Uint8Array.from(w.bytes);
}

export function decodePatch(image) {
  if (image.length < 10) throw new Error('image is too short to be a patch');
  const r = new Reader(image);
  if (r.u32() !== P.PATCH_MAGIC) throw new Error('not an MMMC patch (bad magic)');
  // One version, and nothing else is read: an image from another build is
  // refused rather than decoded into the wrong bytes, exactly as the
  // firmware's own decoder refuses it (src/patch/patch_codec.h).
  const version = r.u8();
  if (version !== P.PATCH_FORMAT_VERSION) {
    throw new Error(`patch format version ${version}; this app speaks ${P.PATCH_FORMAT_VERSION}`);
  }
  r.u8();                        // flags
  const payload = r.u16();
  const crcAt = 8 + payload;
  if (crcAt + 2 > image.length) throw new Error('image ended mid-record');
  const stored = image[crcAt] | (image[crcAt + 1] << 8);
  if (stored !== crc16(image.subarray(0, crcAt))) throw new Error('checksum failed');

  const patch = emptyPatch();
  const globals = readGlobals(r);
  for (let i = 0; i < P.GPIO_N; i++) patch.gatePorts[i] = { direction: r.u8(), buses: r.set() };
  for (let i = 0; i < P.N_MIDI_IN_NODES; i++) {
    patch.midiIn[i] = { sourceMask: r.u8(), channel: r.u8(), buses: r.set() };
  }
  for (let i = 0; i < P.N_MIDI_OUT_NODES; i++) {
    patch.midiOut[i] = { targetMask: r.u8(), channel: r.u8(), buses: r.set() };
  }

  const nNodes = r.u8();
  if (nNodes > P.N_NODE) throw new Error(`${nNodes} nodes; the module holds ${P.N_NODE}`);
  for (let i = 0; i < nNodes; i++) {
    const node = emptyNode(r.u8());
    const nIn = r.u8();
    if (nIn > P.MAX_IN) throw new Error('a node carries more inlets than the module has');
    r.sets(nIn).forEach((set, k) => { node.inBuses[k] = set; });
    const nOut = r.u8();
    if (nOut > P.MAX_OUT) throw new Error('a node carries more outlets than the module has');
    r.sets(nOut).forEach((set, k) => { node.outBuses[k] = set; });
    const nParams = r.u16();
    if (nParams > P.N_PARAM) throw new Error('a node carries more parameters than the module has');
    for (let k = 0; k < nParams; k++) node.params[k] = r.u8();
    patch.nodes.push(node);
  }

  const nMappings = r.u8();
  if (nMappings > P.N_CC_MAP) throw new Error('more controller bindings than the module holds');
  for (let i = 0; i < nMappings; i++) {
    const slot = r.u8();
    const mapping = {
      sourceMask: r.u8(),
      channel: r.u8(),
      cc: r.u8(),
      targetKind: r.u8(),
      targetIndex: r.u8(),
      param: r.u16(),
      min: r.u16(),
      max: r.u16(),
      flags: r.u8(),
    };
    if (slot < P.N_CC_MAP) patch.ccMap[slot] = mapping;
  }

  const nRoutes = r.u8();
  if (nRoutes > P.N_MOD_ROUTE) throw new Error('more modulation routes than the module holds');
  for (let i = 0; i < nRoutes; i++) {
    const slot = r.u8();
    const route = {
      buses: r.set(),
      targetKind: r.u8(),
      targetIndex: r.u8(),
      param: r.u16(),
      min: r.u16(),
      max: r.u16(),
      depth: r.u8(),
      flags: r.u8(),
    };
    if (slot < P.N_MOD_ROUTE) patch.modMap[slot] = route;
  }

  const nMacros = r.u8();
  if (nMacros > P.N_MACRO) throw new Error('more macros than the module holds');
  for (let i = 0; i < nMacros; i++) {
    const slot = r.u8();
    const bytes = Array.from({ length: P.MACRO_NAME_BYTES }, () => r.u8());
    const name = nameFrom(bytes, 0);
    if (slot < P.N_MACRO) patch.macros[slot] = { name };
  }

  const nDests = r.u8();
  if (nDests > P.N_MACRO_DEST) throw new Error('more macro destinations than the module holds');
  for (let i = 0; i < nDests; i++) {
    const slot = r.u8();
    const dest = {
      macro: r.u8(),
      targetKind: r.u8(),
      targetIndex: r.u8(),
      param: r.u16(),
      srcLo: r.u8(),
      srcHi: r.u8(),
      depth: signed16(r.u16()),
      flags: r.u8(),
    };
    if (slot < P.N_MACRO_DEST) patch.macroDest[slot] = dest;
  }

  return { patch, globals };
}

// --- SysEx framing --------------------------------------------------------

export function message(command, args = [], device = P.SYSEX_DEFAULT_DEVICE) {
  return Uint8Array.from([
    0xf0, P.SYSEX_MANUFACTURER, device, command, P.SYSEX_PROTOCOL_VERSION, ...args, 0xf7,
  ]);
}

export function identityRequest(device = P.SYSEX_BROADCAST_DEVICE) {
  return Uint8Array.from([
    0xf0, P.SYSEX_UNIVERSAL_NON_REALTIME, device,
    P.SYSEX_GENERAL_INFORMATION, P.SYSEX_IDENTITY_REQUEST, 0xf7,
  ]);
}

// Splits an image into the chunk messages the module expects.
export function patchChunks(image, device = P.SYSEX_DEFAULT_DEVICE) {
  const out = [];
  for (let at = 0; at < image.length; at += P.SYSEX_CHUNK_PAYLOAD) {
    const slice = image.subarray(at, Math.min(at + P.SYSEX_CHUNK_PAYLOAD, image.length));
    const packed = pack(slice);
    let flags = 0;
    if (at === 0) flags |= P.SYSEX_CHUNK_FIRST;
    if (at + slice.length >= image.length) flags |= P.SYSEX_CHUNK_LAST;
    const seq = (out.length) & 0x7f;
    out.push(message(P.SysexCommand.SYSEX_PATCH_CHUNK_IN,
                     [seq, flags, checksum(packed), ...packed], device));
  }
  return out;
}

// Reassembles a dump from the chunk replies, verifying each checksum.
export function reassemble(replies) {
  const bytes = [];
  for (const reply of replies) {
    if (reply[3] !== P.SysexCommand.SYSEX_PATCH_CHUNK_OUT) continue;
    const end = reply[reply.length - 1] === 0xf7 ? reply.length - 1 : reply.length;
    const packed = reply.subarray(8, end);
    if (checksum(packed) !== reply[7]) throw new Error('a dump chunk failed its checksum');
    for (const b of unpack(packed)) bytes.push(b);
  }
  return Uint8Array.from(bytes);
}

// A .syx file: the host-to-device chunks a librarian saved, back into the
// image. `reassemble` reads the reply shape, which differs only in the
// command byte, so each message is rewritten to it on the way.
export function reassembleFile(bytes) {
  return reassemble(splitSysex(bytes).map((m) => {
    const copy = Uint8Array.from(m);
    if (copy[3] === P.SysexCommand.SYSEX_PATCH_CHUNK_IN) copy[3] = P.SysexCommand.SYSEX_PATCH_CHUNK_OUT;
    return copy;
  }));
}

// A run of complete SysEx messages, split on F0/F7: what a file holds, and
// what the module's reply buffer holds.
export function splitSysex(bytes) {
  const messages = [];
  let start = -1;
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] === 0xf0) start = i;
    else if (bytes[i] === 0xf7 && start >= 0) { messages.push(bytes.subarray(start, i + 1)); start = -1; }
  }
  return messages;
}

// A 14-bit value, as the protocol carries one: low seven bits first.
export const u14 = (value) => [value & 0x7f, (value >> 7) & 0x7f];
// A port's set of buses on the wire: sixteen bits, so three data bytes.
export const u21 = (value) => [value & 0x7f, (value >> 7) & 0x7f, (value >> 14) & 0x7f];
export const wireBuses = (buses) => u21(busMask(buses));
export const readBuses = (bytes, at) =>
  busList(bytes[at] | (bytes[at + 1] << 7) | ((bytes[at + 2] & 0x03) << 14));
export const readU14 = (bytes, at) => bytes[at] | (bytes[at + 1] << 7);
// Thirty-two bits as five data bytes, low septet first: a time in
// microseconds, a subtick count, a word of gate buses (SYSEX_MONITOR).
export const readU32 = (bytes, at) =>
  (bytes[at] | (bytes[at + 1] << 7) | (bytes[at + 2] << 14) | (bytes[at + 3] << 21)
   | ((bytes[at + 4] & 0x0f) << 28)) >>> 0;

// A signed value on the wire: magnitude as a u14, then the sign on its own.
// A data byte has no room for a sign bit, and biasing would halve a range the
// target's own units already fill - see src/protocol/sysex.h at
// SYSEX_SET_MACRO_DEST, which is the one place that decides this.
export const s14 = (value) => [...u14(Math.min(0x3fff, Math.abs(value | 0))), value < 0 ? 1 : 0];
export const readS14 = (bytes, at) => (bytes[at + 2] ? -readU14(bytes, at) : readU14(bytes, at));
