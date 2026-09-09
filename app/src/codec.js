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

import * as P from './protocol.js';

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
    pcEnabled: 0,
    pcChannel: 1,
    pcSourceMask: 0,
    pcQuantise: 0,
    nrpnEnabled: 0,
    nrpnChannel: 0,
    nrpnSourceMask: 0,
  };
}

export function emptyPatch() {
  return {
    gatePorts: Array.from({ length: P.GPIO_N }, () => ({ direction: 0, bus: P.NO_BUS })),
    midiIn: Array.from({ length: P.N_MIDI_IN_NODES }, () => ({ sourceMask: 0, channel: 0, bus: P.NO_BUS })),
    midiOut: Array.from({ length: P.N_MIDI_OUT_NODES }, () => ({ targetMask: 0, channel: 0, bus: P.NO_BUS })),
    nodes: [],
    ccMap: Array.from({ length: P.N_CC_MAP }, () => null),
  };
}

export function emptyNode(algorithmId) {
  return {
    algorithmId,
    inBus: Array.from({ length: P.MAX_IN }, () => P.NO_BUS),
    outBus: Array.from({ length: P.MAX_OUT }, () => P.NO_BUS),
    params: new Uint8Array(P.N_PARAM),
  };
}

class Writer {
  constructor() { this.bytes = []; }
  u8(v) { this.bytes.push(v & 0xff); }
  u16(v) { this.u8(v & 0xff); this.u8((v >> 8) & 0xff); }
  u32(v) { this.u16(v & 0xffff); this.u16((v >>> 16) & 0xffff); }
  many(values) { for (const v of values) this.u8(v); }
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
}

function writeGlobals(w, g) {
  // sizeof(GlobalSettings) bytes, in declaration order, then the reserved tail.
  w.u8(g.clockSource);
  w.u8(g.cvPpqn);
  w.u16(g.bpm);
  w.u8(g.pcEnabled);
  w.u8(g.pcChannel);
  w.u8(g.pcSourceMask);
  w.u8(g.pcQuantise);
  w.u8(g.nrpnEnabled);
  w.u8(g.nrpnChannel);
  w.u8(g.nrpnSourceMask);
  for (let i = 0; i < GLOBALS_BYTES - 11; i++) w.u8(0);
}

function readGlobals(r) {
  const g = {
    clockSource: r.u8(),
    cvPpqn: r.u8(),
    bpm: r.u16(),
    pcEnabled: r.u8(),
    pcChannel: r.u8(),
    pcSourceMask: r.u8(),
    pcQuantise: r.u8(),
    nrpnEnabled: r.u8(),
    nrpnChannel: r.u8(),
    nrpnSourceMask: r.u8(),
  };
  for (let i = 0; i < GLOBALS_BYTES - 11; i++) r.u8();
  return g;
}

// The last non-zero parameter plus one: an algorithm uses the first few bytes
// and leaves the rest zero, and the image does not carry the tail. This is
// what makes a patch fit a 1 KB preset slot when sizeof(Patch) is 11 KB.
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
  for (const port of patch.gatePorts) { w.u8(port.direction); w.u8(port.bus); }
  for (const port of patch.midiIn) { w.u8(port.sourceMask); w.u8(port.channel); w.u8(port.bus); }
  for (const port of patch.midiOut) { w.u8(port.targetMask); w.u8(port.channel); w.u8(port.bus); }

  w.u8(patch.nodes.length);
  for (const node of patch.nodes) {
    w.u8(node.algorithmId);
    w.many(node.inBus);
    w.many(node.outBus);
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
  for (let i = 0; i < P.GPIO_N; i++) patch.gatePorts[i] = { direction: r.u8(), bus: r.u8() };
  for (let i = 0; i < P.N_MIDI_IN_NODES; i++) {
    patch.midiIn[i] = { sourceMask: r.u8(), channel: r.u8(), bus: r.u8() };
  }
  for (let i = 0; i < P.N_MIDI_OUT_NODES; i++) {
    patch.midiOut[i] = { targetMask: r.u8(), channel: r.u8(), bus: r.u8() };
  }

  const nNodes = r.u8();
  if (nNodes > P.N_NODE) throw new Error(`${nNodes} nodes; the module holds ${P.N_NODE}`);
  for (let i = 0; i < nNodes; i++) {
    const node = emptyNode(r.u8());
    node.inBus = r.many(P.MAX_IN);
    node.outBus = r.many(P.MAX_OUT);
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

// A 14-bit value, as the protocol carries one: low seven bits first.
export const u14 = (value) => [value & 0x7f, (value >> 7) & 0x7f];
export const readU14 = (bytes, at) => bytes[at] | (bytes[at + 1] << 7);
