// The module, over Web MIDI.
//
// **The app is a client of the protocol and nothing more.** If it needs
// something the protocol does not have, the protocol gains a message - never a
// private side channel - so the console, this app and an eventual Launchpad
// stay interchangeable.
//
// **The device is the source of truth.** This mirrors device state; it does
// not own it. With no panel controls there is no user-initiated drift to
// reconcile, but the module still originates state on its own - a Program
// Change recall, a CC learn, an error behind the red LED - and those arrive as
// events rather than being polled for.
//
// Everything the app knows about what the firmware *has* - the algorithms,
// their inlets and outlets and domains, every parameter's range and meaning,
// and the module's capacities - is read from the device. An algorithm added to
// the firmware appears here with no change to this file.

import * as P from './generated.js';
import * as codec from './codec.js';

const REPLY_TIMEOUT_MS = 2000;

// A transport is anything that can send a SysEx message and report the ones it
// receives. Web MIDI is one; the wasm emulator in the tests is another, which
// is how the app is tested against the real firmware.
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
    // Replies come back in the order the requests went out: the module
    // answers each message before it reads the next, and a cable keeps them
    // in order. So the oldest request still waiting owns whatever arrives,
    // until it says its exchange is over. Offering a reply to every waiter
    // read the ACK of an edit as the first reply to the monitor request sent
    // in the same breath, and with a frame nearly always in flight that was
    // every other answer.
    while (this.pending.length && this.pending[0].done) this.pending.shift();
    const waiter = this.pending[0];
    if (!waiter) return;
    waiter.offer(bytes);
    if (waiter.done) this.pending.shift();
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
    this.capabilities.ccMappings = u8();
    this.capabilities.modRoutes = u8();
    this.capabilities.cvFull = u14();
    // The macro table and the pool it shares, so a panel draws its budget
    // from the module rather than from eight and thirty-two written into it.
    this.capabilities.macros = u8();
    this.capabilities.macroDests = u8();
    this.capabilities.macroDestsPerMacro = u8();
    this.capabilities.macroNameBytes = u8();
    return this.capabilities;
  }

  // The registry, read rather than hardcoded: an algorithm added to the
  // firmware appears in the app with no change here.
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
        category: P.AlgorithmCategory.CATEGORY_NONE,
        singleton: false,
        readsKey: false,
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
      // What each connection means, and what the algorithm is for. These are
      // the variable-length tail of the record, which is why string() reports
      // the end rather than reading past it into the next message.
      for (let i = 0; i < descriptor.nIn; i++) descriptor.inName.push(string());
      for (let i = 0; i < descriptor.nOut; i++) descriptor.outName.push(string());
      descriptor.summary = string();
      // What kind of thing it is, which is the shelf of the picker's list it
      // goes on. A truncated record leaves CATEGORY_NONE, which files it
      // under "other": the algorithm is still offered.
      if (at < end) descriptor.category = reply[at++];
      // Whether the patch may hold more than one of it. Only the key is one
      // so far (src/algorithm/midi/key.h).
      if (at < end) descriptor.singleton = reply[at++] !== 0;
      // Whether it plays in the key, which is what puts the key on its card:
      // the module says so, so an algorithm that starts reading the key says
      // it once, in its own descriptor.
      if (at < end) descriptor.readsKey = reply[at++] !== 0;
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
      // 14-bit, as the firmware sends them: a range reaching 255 does not fit
      // in a SysEx data byte, and a max reported as 127 makes the validator
      // here refuse a pattern byte with step 8 in it.
      const min = codec.readU14(reply, at); at += 2;
      const max = codec.readU14(reply, at); at += 2;
      const def = codec.readU14(reply, at); at += 2;
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
      // What the algorithm calls this group, after the options and empty for
      // the algorithms with no opinion. The options are variable-length, so
      // where it starts is only known once they have been read.
      const end = reply[reply.length - 1] === 0xf7 ? reply.length - 1 : reply.length;
      let label = '';
      if (at < end) {
        const length = reply[at++];
        label = String.fromCharCode(...reply.subarray(at, at + length));
        at += length;
      }
      groups[groupIndex] ??= { first, repeat, nFields, label, fields: [] };
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

  // The whole set of buses that port is on, not one bus: the message says
  // what the port *is*, so two editors cannot drift apart over it
  // (src/bus/domain.h).
  async setConnection(node, isOutlet, index, buses) {
    return this.command(P.SysexCommand.SYSEX_SET_CONNECTION,
      [node, isOutlet ? 1 : 0, index, ...codec.wireBuses(buses)]);
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
  async setGatePort(jack, direction, buses) {
    return this.command(P.SysexCommand.SYSEX_SET_GATE_PORT,
      [jack, direction, ...codec.wireBuses(buses)]);
  }
  async setMidiPort(index, isOut, mask, channel, buses) {
    const flags = (isOut ? 1 : 0) | ((mask & 0x80) ? 2 : 0);
    return this.command(P.SysexCommand.SYSEX_SET_MIDI_PORT,
      [index, flags, mask & 0x7f, channel, ...codec.wireBuses(buses)]);
  }
  // The key and the register it sits in (src/midi/global_key.h) ride on the
  // same message: they are patch state rather than a node's, so they are set
  // the same way the clock is.
  async setGlobals(g) {
    return this.command(P.SysexCommand.SYSEX_SET_GLOBALS, [
      g.clockSource, g.cvPpqn, ...codec.u14(g.bpm),
      g.pcEnabled, g.pcChannel, g.pcSourceMask, g.pcQuantise,
      g.scale ?? P.ScaleId.SCALE_CHROMATIC, g.root ?? 0, g.rootOctave ?? 0,
    ]);
  }
  // Where the clock arrives and where it leaves. A port mask reaches 0x80 and
  // a data byte holds seven bits, so the top bit of each rides in a third
  // byte (src/protocol/sysex.h).
  async setClockRoute(inMask, outMask) {
    const high = ((inMask & 0x80) ? 0x01 : 0) | ((outMask & 0x80) ? 0x02 : 0);
    return this.command(P.SysexCommand.SYSEX_SET_CLOCK_ROUTE,
                        [inMask & 0x7f, outMask & 0x7f, high]);
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
  // One modulation route (src/control/mod_matrix.h).
  async setModRoute(slot, route) {
    // A route with no source buses is a cleared slot: the module reads the
    // empty set that way rather than needing an enable flag beside it.
    const r = route ?? {};
    if (!(r.buses ?? []).length) {
      return this.command(P.SysexCommand.SYSEX_SET_MOD_ROUTE,
        [slot, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    }
    return this.command(P.SysexCommand.SYSEX_SET_MOD_ROUTE, [
      slot, ...codec.wireBuses(r.buses), r.targetKind, r.targetIndex,
      ...codec.u14(r.param), ...codec.u14(r.min), ...codec.u14(r.max),
      r.depth & 0x7f, (r.flags & 0x3f) | ((r.depth & 0x80) ? 0x40 : 0),
    ]);
  }
  async getModRoute(slot) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_MOD_ROUTE, [slot]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_MOD_ROUTE) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    this.throwOnNak(reply, 'no such modulation route');
    const buses = codec.readBuses(reply, 6);
    if (!buses.length) return null;
    const flags = reply[18];
    return {
      buses,
      targetKind: reply[9],
      targetIndex: reply[10],
      param: codec.readU14(reply, 11),
      min: codec.readU14(reply, 13),
      max: codec.readU14(reply, 15),
      depth: (reply[17] & 0x7f) | ((flags & 0x40) ? 0x80 : 0),
      flags: flags & 0x3f,
    };
  }

  // What a route is *doing* - the signal it is reading, the range it may
  // write, and the value it is holding the target at. The firmware works all
  // of this out once a pass anyway (src/control/mod_matrix.h), so this asks it
  // rather than re-deriving the same arithmetic here: a second implementation
  // of the matrix in JavaScript would be right until the day it was not, and
  // the day it was not is the day somebody is already confused about why a
  // modulation is doing nothing.
  //
  // A module built before this message answers with a NAK, which reads as
  // null: the panel drops the live view and the rest of the editor is
  // unaffected.
  async getModState(slot) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_MOD_STATE, [slot]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_MOD_STATE) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    if (!reply || this.isReply(reply, P.SysexCommand.SYSEX_NAK)) return null;
    return {
      slot: reply[5],
      status: reply[6],
      // Biased by CV_FULL on the wire, because a bus reading is signed and a
      // SysEx data byte is not.
      cv: codec.readU14(reply, 7) - P.CV_FULL,
      position: codec.readU14(reply, 9),
      rangeLo: codec.readU14(reply, 11),
      rangeHi: codec.readU14(reply, 13),
      centre: codec.readU14(reply, 15),
      value: codec.readU14(reply, 17),
    };
  }

  // Any control target, by kind and index, as the module has it **now**
  // (SYSEX_GET_CONTROL). The patch holds what a setting was saved as; this is
  // what it currently is, and the two are different numbers the moment
  // something other than the editor has moved it - a Key node walking the
  // root off a note bus, a CC or an NRPN bound to the key or the clock, a
  // modulation route, a tap on the tempo.
  //
  // A module that does not know the message answers with a NAK, which reads
  // as null: the indicator falls back to the stored value and nothing else in
  // the editor notices.
  async getControl(kind, index, param) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_CONTROL, [kind, index, ...codec.u14(param)]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_CONTROL_VALUE) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    if (!reply || this.isReply(reply, P.SysexCommand.SYSEX_NAK)) return null;
    return codec.readU14(reply, 9);
  }

  // The monitor ------------------------------------------------------------

  // What the module has been doing since it was last asked
  // (src/monitor/monitor.h): the frame everything live on the page is
  // painted from. `noteBuses` is a mask of the buses whose notes are wanted
  // and `nodes` the ones whose positions are, because a bus nobody draws is
  // not read and a playhead is drawn for the card that is open. Asked once
  // per animation frame by the session, and the record starts again each
  // time, so this is the one request that must not be sent twice at once.
  async monitorFrame(noteBuses, nodes = []) {
    const asked = nodes.slice(0, P.MONITOR_MAX_NODES);
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_MONITOR_REQUEST,
               [...codec.u21(noteBuses & 0xffff), asked.length, ...asked]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_MONITOR) || this.isReply(r, P.SysexCommand.SYSEX_NAK),
      { what: 'the monitor' });
    this.throwOnNak(reply, 'the module refused a monitor request');
    return decodeMonitor(reply);
  }

  // Macros ---------------------------------------------------------------
  // A macro is a *target*: nothing here moves one. It is moved by a source -
  // a knob through the binding table, a CV bus through the matrix - so what
  // the app writes is the name and the destinations, and what it reads back
  // is where the module has put it.

  async setMacro(index, name) {
    return this.command(P.SysexCommand.SYSEX_SET_MACRO, [index, ...codec.nameBytes(name)]);
  }
  async getMacro(index) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_MACRO, [index]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_MACRO) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    this.throwOnNak(reply, 'no such macro');
    const name = codec.nameFrom(reply, 6);
    return name ? { name } : null;
  }

  // One destination. A macro index that is not a macro clears the slot, which
  // is what the firmware reads too: MACRO_NONE is 0xFF and does not fit a
  // data byte, so "not a macro" says it rather than a separate enable flag
  // that could disagree with the macro field.
  async setMacroDest(slot, dest) {
    const d = dest ?? null;
    if (!d || d.macro === null || d.macro === undefined || d.macro === codec.MACRO_NONE) {
      return this.command(P.SysexCommand.SYSEX_SET_MACRO_DEST,
        [slot, 0x7f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    }
    return this.command(P.SysexCommand.SYSEX_SET_MACRO_DEST, [
      slot, d.macro, d.targetKind, d.targetIndex,
      ...codec.u14(d.param), ...codec.u14(d.srcLo), ...codec.u14(d.srcHi),
      ...codec.s14(d.depth), d.flags ?? 0,
    ]);
  }
  async getMacroDest(slot) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_MACRO_DEST, [slot]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_MACRO_DEST) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    this.throwOnNak(reply, 'no such macro destination');
    const macro = reply[6];
    if (macro >= 0x7f) return null;
    return {
      macro,
      targetKind: reply[7],
      targetIndex: reply[8],
      param: codec.readU14(reply, 9),
      srcLo: codec.readU14(reply, 11),
      srcHi: codec.readU14(reply, 13),
      depth: codec.readS14(reply, 15),
      flags: reply[18],
    };
  }

  // Where a macro *is*, and what each of its destinations is doing about it.
  // None of this is in the patch and none of it can be derived from the patch:
  // a macro deliberately does not store its position, so a pot on screen has
  // nothing to draw until the module is asked. A destination that has never
  // been moved reads as silent rather than as a mistake, which is the
  // distinction the bench exists to draw.
  async getMacroState(index) {
    const [reply] = await this.request(
      this.msg(P.SysexCommand.SYSEX_GET_MACRO_STATE, [index]),
      (r) => this.isReply(r, P.SysexCommand.SYSEX_MACRO_STATE) || this.isReply(r, P.SysexCommand.SYSEX_NAK));
    if (!reply || this.isReply(reply, P.SysexCommand.SYSEX_NAK)) return null;
    const dests = [];
    const n = reply[9];
    for (let i = 0, at = 10; i < n; i++, at += 9) {
      dests.push({
        slot: reply[at],
        status: reply[at + 1],
        offset: codec.readS14(reply, at + 2),
        anchor: codec.readU14(reply, at + 5),
        value: codec.readU14(reply, at + 7),
      });
    }
    return {
      macro: reply[5],
      engaged: reply[6] !== 0,
      position: codec.readU14(reply, 7),
      dests,
    };
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

  // --- the transport, and the panic button ------------------------------------

  // One of the four buttons a controller can also be bound to
  // (`CcTransportTarget`). It goes over the control cable rather than as MIDI
  // realtime, because realtime bytes arrive on a musical port and the app
  // holds cable 3 - which is reserved and carries nothing musical.
  //
  // Not `transport()`: that is the wire this Device talks over.
  async pressTransport(what) { return this.command(P.SysexCommand.SYSEX_TRANSPORT, [what]); }

  // Everything the module is playing, released: its nodes hand back what they
  // are holding, then All Notes Off on every channel of every port it can
  // play. Only the module can do either - the notes are on its own cables.
  async panic() { return this.command(P.SysexCommand.SYSEX_PANIC); }

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

// One SYSEX_MONITOR frame, byte for byte as src/protocol/sysex.h lays it out.
export function decodeMonitor(reply) {
  let at = 5;
  const u8 = () => reply[at++];
  const u14 = () => { const v = codec.readU14(reply, at); at += 2; return v; };
  const u32 = () => { const v = codec.readU32(reply, at); at += 5; return v; };
  const s14 = () => { const v = codec.readS14(reply, at); at += 3; return v; };
  const frame = { at: u32() };
  const clockFlags = u8();
  frame.clock = { running: (clockFlags & 1) !== 0, bpm: u14(), count: u32() };
  frame.gate = { now: u32(), since: u32() };
  frame.jackIn = { now: u14(), since: u14() };
  frame.jackOut = { now: u14(), since: u14() };
  frame.green = u14();
  frame.red = u14();
  frame.cv = [];
  for (let b = 0; b < P.N_CV_BUS; b++) frame.cv.push(s14());
  frame.nodes = [];
  const nodes = u8();
  for (let i = 0; i < nodes; i++) {
    const node = u8();
    const count = u8();
    const values = [];
    for (let v = 0; v < count; v++) values.push(u8());
    frame.nodes.push({ node, values });
  }
  frame.lost = u8() !== 0;
  frame.events = [];
  const events = u8();
  for (let i = 0; i < events; i++) {
    const where = u8();
    const arg = u8() | ((where & 0x40) ? 0x80 : 0);
    const flags = u8();
    const d1 = u8();
    const d2 = u8();
    const ageMs = u14();
    frame.events.push({
      bus: (where & 0x01) ? null : arg,
      target: (where & 0x01) ? arg : null,
      type: (flags & 0x40) ? 0x90 : 0x80,
      channel: flags & 0x1f,
      d1, d2, ageMs,
    });
  }
  return frame;
}
