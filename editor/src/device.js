// The module, over Web MIDI.
//
// **The editor is a client of the protocol and nothing more.** If it needs
// something the protocol does not have, the protocol gains a message - never a
// private side channel - so the console, this editor and an eventual Launchpad
// stay interchangeable.
//
// **The device is the source of truth.** This mirrors device state; it does
// not own it. With no panel controls there is no user-initiated drift to
// reconcile, but the module still originates state on its own - a Program
// Change recall, a CC learn, an error behind the red LED - and those arrive as
// events rather than being polled for.
//
// Everything the editor knows about what the firmware *has* - the algorithms,
// their inlets and outlets and domains, every parameter's range and meaning,
// and the module's capacities - is read from the device. An algorithm added to
// the firmware appears here with no change to this file.

import * as P from './protocol.js';
import * as codec from './codec.js';

const REPLY_TIMEOUT_MS = 2000;

// A transport is anything that can send a SysEx message and report the ones it
// receives. Web MIDI is one; the wasm emulator in the tests is another, which
// is how the editor is tested against the real firmware.
export class Device extends EventTarget {
  constructor(transport) {
    super();
    this.transport = transport;
    this.deviceId = P.SYSEX_DEFAULT_DEVICE;
    this.capabilities = null;
    this.algorithms = [];          // by table index
    this.byId = new Map();         // algorithm id -> descriptor
    this.pending = [];
    transport.onMessage((bytes) => this.receive(bytes));
  }

  receive(bytes) {
    if (bytes[0] !== 0xf0) return;

    // An unsolicited event: the module speaking first.
    if (bytes[1] === P.SYSEX_MANUFACTURER && bytes[3] === P.SysexCommand.SYSEX_EVENT) {
      this.dispatchEvent(new CustomEvent('device-event', {
        detail: { event: bytes[5], detail: bytes[6] },
      }));
      return;
    }
    for (const waiter of this.pending) waiter.offer(bytes);
    this.pending = this.pending.filter((w) => !w.done);
  }

  send(bytes) { this.transport.send(bytes); }

  // Collects replies until `isDone` says the exchange is over. Every request
  // that expects an answer goes through here, so a module that has gone away
  // produces a clear timeout rather than a hung UI.
  //
  // **A timeout is a failure, including when replies did arrive.** Resolving
  // with a partial answer looks forgiving and is not: it turns a disagreement
  // about the message layout into a two-second stall and a half-built panel,
  // which is exactly the bug this comment exists because of. `partialOk` is
  // for the one caller that genuinely wants whatever turned up.
  request(bytes, isDone, { timeout = REPLY_TIMEOUT_MS, partialOk = false, what = 'the module' } = {}) {
    return new Promise((resolve, reject) => {
      const replies = [];
      const waiter = {
        done: false,
        offer(reply) {
          replies.push(reply);
          if (!isDone(reply, replies)) return;
          waiter.done = true;
          clearTimeout(timer);
          resolve(replies);
        },
      };
      const timer = setTimeout(() => {
        waiter.done = true;
        if (partialOk && replies.length) { resolve(replies); return; }
        reject(new Error(replies.length
          ? `${what}: the answer never completed (${replies.length} messages in ${timeout}ms)`
          : `${what} did not answer`));
      }, timeout);
      this.pending.push(waiter);
      this.send(bytes);
      this.transport.flush?.();
    });
  }

  isReply(bytes, command) {
    return bytes[1] === P.SYSEX_MANUFACTURER && bytes[3] === command;
  }

  // The standard identity request, so a user does not have to pick a port by
  // name. The module blinks both LEDs when it answers, which is the only way
  // to tell two modules apart.
  async identify() {
    const replies = await this.request(
      codec.identityRequest(),
      (r) => r[1] === P.SYSEX_UNIVERSAL_NON_REALTIME && r[4] === P.SYSEX_IDENTITY_REPLY);
    const reply = replies.find((r) => r[1] === P.SYSEX_UNIVERSAL_NON_REALTIME);
    if (!reply) throw new Error('no identity reply');
    if (reply[5] !== P.SYSEX_MANUFACTURER) throw new Error('not an MMMC');
    this.deviceId = reply[2];
    return { device: reply[2], family: reply[6], member: reply[8] };
  }

  async readCapabilities() {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_CAPS_REQUEST),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_CAPABILITIES),
      { what: 'capabilities' });
    let at = 5;
    const u8 = () => reply[at++];
    const u14 = () => { const v = codec.readU14(reply, at); at += 2; return v; };
    this.capabilities = {
      nodes: u8(), maxIn: u8(), maxOut: u8(), nParams: u14(),
      gateBuses: u8(), noteBuses: u8(), cvBuses: u8(),
      jacks: u8(), midiIn: u8(), midiOut: u8(),
      algorithms: u8(), slots: u8(), slotBytes: u14(),
      sequenceLength: u8(), voices: u8(), drumLanes: u8(),
      ppqn: u8(), controlPort: u8(),
    };
    return this.capabilities;
  }

  // The registry, read rather than hardcoded: an algorithm added to the
  // firmware appears in the editor with no editor change.
  async readAlgorithms() {
    const replies = await this.request(
      this.msg(P.SysexCommand.SYSEX_ALGO_REQUEST),
      (r, all) => this.isReply(r, P.SysexCommand.SYSEX_ALGORITHM)
                  && all.filter((x) => this.isReply(x, P.SysexCommand.SYSEX_ALGORITHM)).length >= r[6],
      { what: 'the algorithm list' });
    this.algorithms = [];
    this.byId.clear();
    for (const reply of replies) {
      if (!this.isReply(reply, P.SysexCommand.SYSEX_ALGORITHM)) continue;
      let at = 5;
      const u8 = () => reply[at++];
      const index = u8();
      u8();                                   // the total, already used above
      const descriptor = {
        index,
        id: u8(),
        nIn: u8(),
        minIn: u8(),
        nOut: u8(),
        nParams: 0,
        wantsTick: 0,
        inDomain: [],
        outDomain: [],
        inName: [],                            // null where the module did not say
        outName: [],
        summary: null,
        name: '',
        params: null,                          // filled in lazily by readParams
      };
      descriptor.nParams = codec.readU14(reply, at); at += 2;
      descriptor.wantsTick = reply[at++];
      for (let i = 0; i < descriptor.nIn; i++) descriptor.inDomain.push(reply[at++]);
      for (let i = 0; i < descriptor.nOut; i++) descriptor.outDomain.push(reply[at++]);
      const end = reply[reply.length - 1] === 0xf7 ? reply.length - 1 : reply.length;
      const string = () => {
        if (at >= end) return null;
        const length = reply[at++];
        if (at + length > end) { at = end; return null; }
        const text = String.fromCharCode(...reply.subarray(at, at + length));
        at += length;
        return text;
      };
      descriptor.name = string() ?? `algorithm ${descriptor.id}`;
      // What each connection means, and what the algorithm is for. These come
      // after the name, so a module whose firmware predates them simply runs
      // out of record here and the editor falls back to the index - the
      // reason string() reports the end rather than reading past it.
      for (let i = 0; i < descriptor.nIn; i++) descriptor.inName.push(string());
      for (let i = 0; i < descriptor.nOut; i++) descriptor.outName.push(string());
      descriptor.summary = string();
      this.algorithms[index] = descriptor;
      this.byId.set(descriptor.id, descriptor);
    }
    return this.algorithms;
  }

  // Parameter descriptors, as the ParamGroup runs the firmware stores them
  // as: a poly sequencer's 336 parameters are a handful of messages.
  async readParams(algorithmId) {
    const descriptor = this.byId.get(algorithmId);
    if (!descriptor) throw new Error(`no algorithm ${algorithmId}`);
    if (descriptor.params) return descriptor.params;

    const replies = await this.request(
      this.msg(P.SysexCommand.SYSEX_PARAM_REQUEST, [algorithmId]),
      (r) => {
        // An algorithm with no parameters - a logic gate - answers with an
        // ACK rather than nothing, so a silent module is still a timeout.
        if (this.isReply(r, P.SysexCommand.SYSEX_ACK)) return true;
        if (this.isReply(r, P.SysexCommand.SYSEX_NAK)) return true;
        if (!this.isReply(r, P.SysexCommand.SYSEX_PARAM_DESC)) return false;
        // Otherwise the last field of the last group ends the exchange. The
        // offsets are the reply's own layout: 6 group, 7 group count, 8 first,
        // 10 repeat, 12 field count, 14 field index - each u14 taking two.
        const groups = r[7];
        const fields = codec.readU14(r, 12);
        const field = codec.readU14(r, 14);
        return r[6] === groups - 1 && field === fields - 1;
      },
      { what: `the parameters of algorithm ${algorithmId}` });
    if (replies.some((r) => this.isReply(r, P.SysexCommand.SYSEX_NAK))) {
      this.throwOnNak(replies.find((r) => this.isReply(r, P.SysexCommand.SYSEX_NAK)),
                      'no such algorithm');
    }

    const groups = [];
    for (const reply of replies) {
      if (!this.isReply(reply, P.SysexCommand.SYSEX_PARAM_DESC)) continue;
      let at = 6;
      const groupIndex = reply[at++];
      at++;                                     // the group count
      const first = codec.readU14(reply, at); at += 2;
      const repeat = codec.readU14(reply, at); at += 2;
      const nFields = codec.readU14(reply, at); at += 2;
      const fieldIndex = codec.readU14(reply, at); at += 2;
      const min = reply[at++];
      const max = reply[at++];
      const def = reply[at++];
      const kind = reply[at++];
      const nameLength = reply[at++];
      const name = String.fromCharCode(...reply.subarray(at, at + nameLength));
      at += nameLength;
      const nOptions = reply[at++];
      const options = [];
      for (let o = 0; o < nOptions; o++) {
        const length = reply[at++];
        options.push(String.fromCharCode(...reply.subarray(at, at + length)));
        at += length;
      }
      groups[groupIndex] ??= { first, repeat, nFields, fields: [] };
      groups[groupIndex].fields[fieldIndex] = { name, min, max, def, kind, options };
    }
    descriptor.params = groups;
    return groups;
  }

  // The descriptor for one parameter index, the same lookup param_lookup()
  // does on the module.
  describeParam(algorithmId, index) {
    const descriptor = this.byId.get(algorithmId);
    if (!descriptor?.params) return null;
    for (const group of descriptor.params) {
      if (!group) continue;
      const span = group.repeat * group.nFields;
      if (index < group.first || index >= group.first + span) continue;
      return group.fields[(index - group.first) % group.nFields];
    }
    return null;
  }

  msg(command, args = []) { return codec.message(command, args, this.deviceId); }

  // Bulk ---------------------------------------------------------------

  async dump() {
    const replies = await this.request(
      this.msg(P.SysexCommand.SYSEX_DUMP_REQUEST),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_PATCH_CHUNK_OUT)
             && (r[6] & P.SYSEX_CHUNK_LAST) !== 0,
      { timeout: 5000 });
    return codec.decodePatch(codec.reassemble(replies));
  }

  // A whole patch, chunked. The module stages it and only swaps when the last
  // chunk has arrived and the image has passed magic, version, CRC and the
  // validator, so a failure here leaves what is running alone.
  async sendPatch(patch, globals) {
    const image = codec.encodePatch(patch, globals);
    const chunks = codec.patchChunks(image, this.deviceId);
    for (let i = 0; i < chunks.length; i++) {
      const last = i === chunks.length - 1;
      const [reply] = await this.request(
        chunks[i],
        (r) => this.isReply(r, P.SysexCommand.SYSEX_ACK) || this.isReply(r, P.SysexCommand.SYSEX_NAK),
        { timeout: last ? 5000 : REPLY_TIMEOUT_MS });
      this.throwOnNak(reply, last ? 'the module rejected the patch' : 'a chunk was refused');
    }
  }

  // Incremental ----------------------------------------------------------
  // Dragging a connection sends one message, not a full dump.

  async setConnection(node, isOutlet, index, bus) {
    return this.command(P.SysexCommand.SYSEX_SET_CONNECTION,
      [node, isOutlet ? 1 : 0, index, bus === P.NO_BUS ? 0x7f : bus]);
  }
  // A parameter byte reaches 255 and a SysEx data byte holds seven bits, so
  // the value travels as a u14 - low seven bits where they have always been,
  // the eighth appended. Truncating it instead is not a rounding error: the
  // high byte of a step pattern *is* step 8, so a truncated write turns the
  // step off and clears the other seven with it.
  async setParam(node, param, value) {
    return this.command(P.SysexCommand.SYSEX_SET_PARAM,
      [node, ...codec.u14(param), ...codec.u14(value & 0xff)]);
  }
  async getParam(node, param) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_PARAM, [node, ...codec.u14(param)]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_PARAM_VALUE) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    this.throwOnNak(reply, 'no such parameter');
    return codec.readU14(reply, 8) & 0xff;
  }
  async setGatePort(jack, direction, bus) {
    return this.command(P.SysexCommand.SYSEX_SET_GATE_PORT,
      [jack, direction, bus === P.NO_BUS ? 0x7f : bus]);
  }
  async setMidiPort(index, isOut, mask, channel, bus) {
    const flags = (isOut ? 1 : 0) | ((mask & 0x80) ? 2 : 0);
    return this.command(P.SysexCommand.SYSEX_SET_MIDI_PORT,
      [index, flags, mask & 0x7f, channel, bus === P.NO_BUS ? 0x7f : bus]);
  }
  async setGlobals(g) {
    return this.command(P.SysexCommand.SYSEX_SET_GLOBALS, [
      g.clockSource, g.cvPpqn, ...codec.u14(g.bpm),
      g.pcEnabled, g.pcChannel, g.pcSourceMask, g.pcQuantise,
    ]);
  }
  async setNrpn(enabled, channel, mask) {
    return this.command(P.SysexCommand.SYSEX_SET_NRPN, [enabled ? 1 : 0, channel, mask]);
  }

  // Pattern data: a note sequencer's grid is far too wide for NRPN, so it
  // travels in runs of parameter bytes.
  async setPattern(node, offset, bytes) {
    return this.command(P.SysexCommand.SYSEX_SET_PATTERN,
      [node, ...codec.u14(offset), ...codec.pack(bytes)]);
  }
  async getPattern(node, offset, length) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_PATTERN, [node, ...codec.u14(offset), length]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_PATTERN) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    this.throwOnNak(reply, 'no such node');
    const end = reply[reply.length - 1] === 0xf7 ? reply.length - 1 : reply.length;
    return codec.unpack(reply.subarray(9, end));
  }

  // Controller bindings ---------------------------------------------------

  async setCcMap(slot, m) {
    return this.command(P.SysexCommand.SYSEX_SET_CC_MAP, [
      slot, m.sourceMask & 0x7f, m.channel, m.cc, m.targetKind, m.targetIndex,
      ...codec.u14(m.param), ...codec.u14(m.min),
      (m.flags & 0x3f) | ((m.sourceMask & 0x80) ? 0x40 : 0),
      ...codec.u14(m.max),
    ]);
  }
  async learnCc(slot, targetKind, targetIndex, param) {
    return this.command(P.SysexCommand.SYSEX_CC_LEARN,
      [1, slot, targetKind, targetIndex, ...codec.u14(param)]);
  }
  async cancelLearn() { return this.command(P.SysexCommand.SYSEX_CC_LEARN, [0]); }

  // Presets ---------------------------------------------------------------

  async slots() {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_SLOT_LIST),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_SLOTS));
    const count = reply[5];
    const out = [];
    for (let s = 0; s < count; s++) {
      out.push({ slot: s, occupied: reply[6 + s * 3] === 1, bytes: codec.readU14(reply, 7 + s * 3) });
    }
    return out;
  }
  async saveSlot(slot) { return this.command(P.SysexCommand.SYSEX_SLOT_SAVE, [slot]); }
  async loadSlot(slot) { return this.command(P.SysexCommand.SYSEX_SLOT_LOAD, [slot]); }
  async eraseSlot(slot) { return this.command(P.SysexCommand.SYSEX_SLOT_ERASE, [slot]); }
  async restoreDefaults() { return this.command(P.SysexCommand.SYSEX_RESTORE_DEFAULTS); }

  async command(command, args = []) {
    const [reply] = await this.request(
      this.msg(command, args),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_ACK) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    this.throwOnNak(reply);
    return true;
  }

  throwOnNak(reply, context = 'the module refused it') {
    if (!reply) throw new Error('the module did not answer');
    if (this.isReply(reply, P.SysexCommand.SYSEX_NAK)) {
      const name = P.SysexErrorName[reply[5]] ?? reply[5];
      throw new Error(`${context}: ${name}`);
    }
  }
}
