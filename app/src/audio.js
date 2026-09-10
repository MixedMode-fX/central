// Hearing the patch.
//
// None of this is firmware. It is a small Web Audio synth standing in for
// whatever would be downstream of the module: a patch you can only read is a
// patch you cannot tell is wrong, and the module has no audio path of its own
// to borrow - so the page provides the ears.
//
// **What it listens to is a choice, and there can be several.** There are two
// kinds of thing worth hearing, and the first one used to be the only one:
//
//   * **what the module sends** - the MIDI leaving a MIDI output node, which
//     is what a synth on the other end of the cable would receive. Complete,
//     and useless while a patch is being built: a bus only leaves the module
//     once somebody has patched a MIDI out to it, so a sequencer feeding an
//     arpeggiator feeding nothing is silent no matter how right it is.
//   * **a note bus** - the patch's own signal, read straight off the bus. Any
//     bus, patched to an output or not, which is what makes an unfinished
//     patch audible at all.
//
// So a *player* is one voice pointed at one source, and there is a list of
// them: the sequencer on note bus 0 through a saw, the arpeggiator on bus 2
// through a square, each at its own level, because "which of these two is
// wrong" is a question about hearing them apart.
//
// **Drums are not players.** A drum sequencer is an instrument, not a source
// somebody might point a sawtooth at: it plays a kit, and which kit and how
// loud is a property of *that sequencer* rather than of whoever is listening
// to it. So every drum sequencer in the patch gets a voice of its own here -
// its own kit, its own level - and a drum note is played there rather than by
// the player that carried it, whichever bus or cable it arrived on. Two drum
// machines in one patch are two instruments, which is what makes "which of
// these two is the one I can hear" answerable at all. `drums.js` is the voice
// itself; this file is what points it at the patch.
//
// **The gate listener is the third thing.** A gate carries no note and no
// velocity, so a clock division, a Euclidean pattern or a logic gate makes no
// MIDI at all and cannot be heard as music - but it can be heard as
// percussion, one blip per rising edge. What it listens to is a choice too,
// and both kinds of gate are on offer: the module's own **gate buses**, which
// is the signal itself, and the **jacks**, which is that signal where it
// leaves the module. It has its own level because it is the loudest thing in
// the page and usually wants to be under the notes.
//
// Events are scheduled on the audio clock at the *simulated* time they
// happened, anchored once per animation frame, so an arpeggio at 10 Hz sounds
// like one even though the passes that produced it ran in a burst.

import * as P from './protocol.js';
import { DEFAULT_KIT, KITS, hit as drumHit, kitLabel, pieceOf } from './drums.js';

const NOTE_OFF = 0x80, NOTE_ON = 0x90, CONTROL_CHANGE = 0xb0, PITCH_BEND = 0xe0;
const SUSTAIN = 64, ALL_SOUND_OFF = 120, ALL_NOTES_OFF = 123;
const DRUM_CHANNEL = 10;
const MAX_VOICES = 24;                 // per player
const MAX_PLAYERS = 6;
const MAX_GATE_SOURCES = 8;
const LATENCY = 0.03;

// The drum voice for anything on channel 10 that no drum sequencer in the
// patch accounts for: the on-screen keyboard, a controller, a note bus written
// by something else entirely. It is a kit like any other, so a patch with no
// drum sequencer in it still gets drums that sound like drums.
export const OTHER_DRUMS = 'other';

export const WAVES = ['sawtooth', 'square', 'triangle', 'sine'];

// One voice pointed at one source. Its own gain, its own notes, its own
// sustain and bend state: two players on two buses are two instruments, not
// one instrument hearing twice as much.
class Player {
  constructor(listener, { source = 'out', bus = 0, wave = 'sawtooth', volume = 1 } = {}) {
    this.listener = listener;
    this.id = `v${Math.random().toString(36).slice(2, 8)}`;
    this.source = source === 'bus' ? 'bus' : 'out';
    this.bus = bus;
    this.wave = WAVES.includes(wave) ? wave : 'sawtooth';
    this.volume = volume;
    this.gain = null;
    this.voices = new Map();
    this.sustain = {};
    this.pending = new Set();
    this.bend = {};
  }

  get ctx() { return this.listener.ctx; }
  get enabled() { return this.listener.enabled; }

  // The gain node cannot exist before the context does, and the context does
  // not exist until somebody presses "enable audio" - browsers only start
  // audio from a gesture. So a player is configured first and wired up later.
  attach() {
    if (this.gain || !this.ctx) return;
    this.gain = this.ctx.createGain();
    this.gain.gain.value = this.volume;
    this.gain.connect(this.listener.master);
  }

  setVolume(value) {
    this.volume = value;
    if (this.gain) this.gain.gain.value = value;
  }

  setWave(wave) {
    if (WAVES.includes(wave)) this.wave = wave;
  }

  // Whose events this player wants. A bus player takes one bus; an out player
  // takes everything leaving the module, on any port, because "what is on the
  // cable" is one thing to a listener.
  wants(event) {
    return this.source === 'bus' ? event.bus === this.bus : event.bus === undefined;
  }

  describe() {
    return this.source === 'bus' ? `note bus ${this.bus}` : 'what the module sends';
  }

  toJSON() {
    return { source: this.source, bus: this.bus, wave: this.wave, volume: this.volume };
  }

  handle(event) {
    if (!this.enabled) return;
    this.attach();
    const { type, d1, d2, channel } = event;
    const at = this.listener.when(event.t);
    if (type === NOTE_ON && d2 > 0) this.noteOn(channel, d1, d2, at);
    else if (type === NOTE_OFF || type === NOTE_ON) this.noteOff(channel, d1, at);
    else if (type === CONTROL_CHANGE && d1 === SUSTAIN) {
      this.sustain[channel] = d2 >= 64;
      if (d2 < 64) {
        for (const key of [...this.pending]) if (Math.floor(key / 128) === channel) this.release(key, at);
      }
    } else if (type === CONTROL_CHANGE && (d1 === ALL_SOUND_OFF || d1 === ALL_NOTES_OFF)) {
      for (const [key, voice] of this.voices) if (voice.channel === channel) this.release(key, at);
    } else if (type === PITCH_BEND) {
      this.bend[channel] = (((d2 << 7) | d1) - 8192) / 8192 * 200;      // ±2 semitones, in cents
      for (const voice of this.voices.values()) {
        if (voice.channel === channel) voice.osc.detune.setValueAtTime(this.bend[channel], at);
      }
    }
  }

  // A drum note never gets here: the listener takes it out of the stream
  // before any player is offered it and plays it on the drum voice its
  // sequencer owns. A player is a pitched voice, and a pitched voice is the
  // wrong instrument for a kick drum however it is enveloped.
  noteOn(channel, note, velocity, at) {
    const ctx = this.ctx;
    const key = channel * 128 + note;
    if (this.voices.has(key)) this.release(key, at);
    if (this.voices.size >= MAX_VOICES) this.release(this.voices.keys().next().value, at);

    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = this.wave;
    osc.frequency.value = 440 * Math.pow(2, (note - 69) / 12);
    osc.detune.value = this.bend[channel] ?? 0;
    gain.gain.setValueAtTime(0, at);
    gain.gain.linearRampToValueAtTime(0.22 * velocity / 127, at + 0.005);
    osc.connect(gain); gain.connect(this.gain);
    osc.start(at);
    this.voices.set(key, { osc, g: gain, channel });
  }

  noteOff(channel, note, at) {
    const key = channel * 128 + note;
    if (!this.voices.has(key)) return;
    if (this.sustain[channel]) this.pending.add(key); else this.release(key, at);
  }

  release(key, at) {
    const voice = this.voices.get(key);
    if (!voice) return;
    voice.g.gain.setTargetAtTime(0, at, 0.04);
    voice.osc.stop(at + 0.4);
    this.voices.delete(key);
    this.pending.delete(key);
  }

  allOff(at) {
    for (const key of [...this.voices.keys()]) this.release(key, at);
  }
}

// One drum sequencer's instrument: a kit, a level, and its own gain so the
// level means something against the notes rather than inside them.
//
// A voice outlives the patch it was made for. Its key is the node's index -
// the same identity the canvas remembers a dragged block by - and the setting
// is kept for that key whether or not the node is in the patch on screen at
// the moment, so loading a patch back does not lose the kit chosen for it.
class DrumVoice {
  constructor(listener, { key, label = 'drums', kind = 'note', kit = DEFAULT_KIT, volume = 0.8, on = true } = {}) {
    this.listener = listener;
    this.key = key;
    this.label = label;
    this.kind = kind;
    this.kit = KITS.some((k) => k.id === kit) ? kit : DEFAULT_KIT;
    this.volume = volume;
    this.on = on !== false;
    this.gain = null;
    this.hits = 0;               // what the play tab shows as activity
  }

  get ctx() { return this.listener.ctx; }

  attach() {
    if (this.gain || !this.ctx) return;
    this.gain = this.ctx.createGain();
    this.gain.gain.value = this.volume;
    this.gain.connect(this.listener.master);
  }

  setKit(kit) {
    if (KITS.some((k) => k.id === kit)) this.kit = kit;
  }

  setVolume(value) {
    this.volume = value;
    if (this.gain) this.gain.gain.value = value;
  }

  setOn(on) { this.on = Boolean(on); }

  describe() { return `${this.label} · ${kitLabel(this.kit)}`; }

  // One hit. `piece` is which drum - from the note number for a MIDI drum
  // sequencer, from the lane for a gate one - and `note` only matters for a
  // note number General MIDI has no name for.
  play(piece, { velocity = 100, note = null, at = 0 } = {}) {
    if (!this.on || !this.listener.enabled) return;
    this.attach();
    if (!this.gain) return;
    this.hits++;
    drumHit(this.ctx, this.gain, { kit: this.kit, piece, note, velocity, at });
  }

  toJSON() {
    return { kit: this.kit, volume: this.volume, on: this.on };
  }
}

// What a gate source is, said in words, for a selector and for a hint.
export function gateSourceLabel(source) {
  if (source.kind === 'jacks') return 'every output jack';
  if (source.kind === 'jack') return `jack ${source.index + 1}`;
  return `gate bus ${source.index}`;
}

// Which of the chosen gate sources fired, given the edges of this pass. Pure,
// and exported for that reason: which bit lit which blip is exactly the part
// that can be wrong in a way no amount of listening would localise.
//
// `rising` carries three masks - the output jacks that went high, every jack
// that went high whichever way it faces, and the gate buses that went high.
// One blip per thing that fired, however many sources asked for it: "every
// output jack" and "jack 3" chosen together are one selection of jack 3, not
// two clicks on top of each other.
export function gateHits(sources, rising) {
  const hits = [];
  const seen = new Set();
  const add = (kind, index) => {
    const id = `${kind}:${index}`;
    if (seen.has(id)) return;
    seen.add(id);
    hits.push({ kind, index });
  };
  for (const source of sources) {
    if (source.kind === 'jacks') {
      for (let j = 0; j < P.GPIO_N; j++) if ((rising.jacksOut >> j) & 1) add('jack', j);
    } else if (source.kind === 'jack') {
      if ((rising.jacks >> source.index) & 1) add('jack', source.index);
    } else if (source.kind === 'bus') {
      if ((rising.buses >> source.index) & 1) add('bus', source.index);
    }
  }
  return hits;
}

export class Listener {
  constructor(module) {
    this.module = module;
    this.ctx = null;
    this.master = null;
    this.clickGain = null;
    this.volume = 0.4;
    this.clicks = true;
    this.clickVolume = 0.5;
    // What the gate listener is pointed at. It opens on what it always did -
    // a blip on every output jack - and everything else is something a user
    // goes and asks for.
    this.gateSources = [{ id: gateId(), kind: 'jacks', index: 0 }];
    this.players = [];
    // Drum voices by node key, plus the one for everything on channel 10 that
    // no drum sequencer in the patch explains.
    this.drums = new Map();
    this.drums.set(OTHER_DRUMS, new DrumVoice(this, {
      key: OTHER_DRUMS, label: 'anything else on channel 10', kind: 'other',
    }));
    this.drumSources = [];
    this.noteRoute = new Map();     // note bus -> drum voice key
    this.gateRoute = new Map();     // gate bus -> { key, piece }
    this.drumOutMask = 0;           // MIDI targets already heard on a bus
    this.watched = new Set();       // note buses this listener is reading itself
    this.frameSim = 0;
    this.frameAudio = 0;
    this.lastJackOut = 0;
    this.lastJackAny = 0;
    this.lastGate = 0;
    module.onMidi((event) => this.midi(event));
    module.onNoteBus((event) => this.midi(event));
    module.onFrame((now) => this.anchor(now));
    module.onPass(() => this.edges());
    // What it opens with is what it has always played: the MIDI leaving the
    // module. A note bus is something a user goes and asks for.
    this.addPlayer({ source: 'out' });
  }

  get enabled() { return this.ctx !== null && this.ctx.state === 'running'; }

  voiceCount() {
    let count = 0;
    for (const player of this.players) count += player.voices.size;
    return count;
  }

  // --- the players ---------------------------------------------------------

  addPlayer(config = {}) {
    if (this.players.length >= MAX_PLAYERS) return null;
    const player = new Player(this, config);
    this.players.push(player);
    if (player.source === 'bus') this.module.watchNoteBus(player.bus);
    player.attach();
    return player;
  }

  removePlayer(id) {
    const index = this.players.findIndex((p) => p.id === id);
    if (index < 0) return;
    const [player] = this.players.splice(index, 1);
    if (player.source === 'bus') this.module.unwatchNoteBus(player.bus);
    if (this.ctx) player.allOff(this.ctx.currentTime);
    player.gain?.disconnect();
  }

  // Pointing a player somewhere else. The old bus is released - both the
  // module's watch on it and whatever it had sounding, which would otherwise
  // hang for ever because its note off is now going somewhere else.
  setPlayerSource(id, source, bus = 0) {
    const player = this.players.find((p) => p.id === id);
    if (!player) return;
    if (player.source === 'bus') this.module.unwatchNoteBus(player.bus);
    if (this.ctx) player.allOff(this.ctx.currentTime);
    player.source = source === 'bus' ? 'bus' : 'out';
    player.bus = bus;
    if (player.source === 'bus') this.module.watchNoteBus(player.bus);
  }

  player(id) { return this.players.find((p) => p.id === id) ?? null; }

  // --- the drum sequencers -------------------------------------------------

  // The patch's drum sequencers, as `drums.js` read them out of it. Called on
  // every render, which is every edit, so a lane dragged onto another bus is
  // heard from that bus on the next pass.
  //
  // The buses a drum sequencer speaks on are read *by this listener*, not by a
  // player: a drum sequencer is audible because it is in the patch, the same
  // way it is visible because it is in the patch. Nobody should have to add a
  // player to hear whether the kick is on the one.
  setDrumSources(sources) {
    this.drumSources = sources;
    const wanted = new Set();
    this.noteRoute = new Map();
    this.gateRoute = new Map();
    for (const source of sources) {
      const voice = this.drumVoice(source.key, source);
      voice.label = source.label;
      voice.kind = source.kind;
      if (source.kind === 'note') {
        if (source.bus === P.NO_BUS) continue;
        wanted.add(source.bus);
        this.noteRoute.set(source.bus, source.key);
      } else {
        for (const lane of source.lanes) this.gateRoute.set(lane.bus, { key: source.key, piece: lane.piece });
      }
    }
    for (const bus of [...this.watched]) {
      if (wanted.has(bus)) continue;
      this.module.unwatchNoteBus(bus);
      this.watched.delete(bus);
    }
    for (const bus of wanted) {
      if (this.watched.has(bus)) continue;
      this.module.watchNoteBus(bus);
      this.watched.add(bus);
    }
  }

  // A node removed renumbers every node after it, and a drum voice is keyed by
  // node index like everything else addressed here - so the kit chosen for
  // DrumSeqMidi 3 moves down with it, or it silently becomes the kit for
  // whatever arrives at index 3 next. Exactly `forgetNode` in `layout.js`,
  // which does this for where a block was dragged to, for the same reason.
  forgetDrumNode(index) {
    const moved = new Map();
    for (const [key, voice] of this.drums) {
      const match = /^node:(\d+)$/.exec(key);
      if (!match) { moved.set(key, voice); continue; }
      const was = Number(match[1]);
      if (was === index) { voice.gain?.disconnect(); continue; }
      voice.key = was > index ? `node:${was - 1}` : key;
      moved.set(voice.key, voice);
    }
    this.drums = moved;
  }

  // Which MIDI targets carry drums that are already being heard on their bus.
  // A drum sequencer patched to a MIDI out sends the same hit twice as far as
  // this page is concerned - once on the bus it writes, once on the cable -
  // and playing both is a flam nobody programmed.
  setDrumOutMask(mask) { this.drumOutMask = mask | 0; }

  // The voice for a key, made if this is the first time it has been asked
  // for. Settings restored from a previous session are already in the map
  // under their key, which is what makes a kit survive a reload.
  drumVoice(key, { label, kind } = {}) {
    let voice = this.drums.get(key);
    if (!voice) {
      voice = new DrumVoice(this, { key, label, kind });
      voice.attach();
      this.drums.set(key, voice);
    }
    return voice;
  }

  // The voices worth showing: the patch's drum sequencers in patch order, and
  // the catch-all last. A voice for a node the patch no longer has is kept -
  // its kit is a preference, and patches come back - but it is not listed.
  drumRows() {
    const rows = [];
    for (const source of this.drumSources) {
      const voice = this.drums.get(source.key);
      if (voice) rows.push({ source, voice });
    }
    rows.push({ source: null, voice: this.drums.get(OTHER_DRUMS) });
    return rows;
  }

  // The drum sequencer that wrote this event, if one did. A note bus a drum
  // sequencer writes is drums whatever channel it is on, because the sequencer
  // says so - and it is heard whether or not anybody is listening to that bus,
  // which is the point: a drum sequencer is audible because it is in the
  // patch.
  drumVoiceFor(event) {
    if (event.bus === undefined) return null;
    const key = this.noteRoute.get(event.bus);
    return key ? this.drums.get(key) : null;
  }

  // A drum that is nobody's sequencer: channel 10, the one convention there
  // is. This one *is* only heard where somebody is listening - a player on
  // that bus, or the player on what the module sends - because a note bus is
  // read for the piano roll as well, and a page that made a noise for every
  // bus it was drawing would be playing patches nobody asked to hear.
  //
  // A hit already heard on the bus that produced it is dropped rather than
  // played twice: a drum sequencer patched to a MIDI out sends the same hit
  // down the cable, and two of them is a flam nobody programmed.
  otherDrumFor(event) {
    if (event.channel !== DRUM_CHANNEL) return null;
    if (event.bus === undefined && event.target !== undefined && (event.target & this.drumOutMask)) return null;
    return this.drums.get(OTHER_DRUMS);
  }

  // --- the context ---------------------------------------------------------

  // Browsers only start audio from a user gesture, so this is a button and
  // never something the page does on its own.
  async toggle() {
    // A context created inside a click starts *running* in Chrome, and the
    // press that created it is the press asking for audio - so treating that
    // first press as a toggle suspended it again and the button did nothing
    // until it was pressed twice.
    const created = !this.ctx;
    if (created) {
      const Context = globalThis.AudioContext ?? globalThis.webkitAudioContext;
      if (!Context) throw new Error('this browser has no Web Audio');
      this.ctx = new Context();
      this.master = this.ctx.createGain();
      this.master.gain.value = this.volume;
      this.master.connect(this.ctx.destination);
      // The clicks hang off their own gain rather than sharing the players':
      // a jack edge is percussion and the notes are music, and the whole point
      // of a separate level is being able to turn one down under the other.
      this.clickGain = this.ctx.createGain();
      this.clickGain.gain.value = this.clickVolume;
      this.clickGain.connect(this.master);
      for (const player of this.players) player.attach();
      for (const voice of this.drums.values()) voice.attach();
    }
    if (!created && this.ctx.state === 'running') {
      this.allOff(this.ctx.currentTime);
      await this.ctx.suspend();
    } else {
      await this.ctx.resume();
    }
    return this.enabled;
  }

  setVolume(value) {
    this.volume = value;
    if (this.master) this.master.gain.value = value;
  }

  setClickVolume(value) {
    this.clickVolume = value;
    if (this.clickGain) this.clickGain.gain.value = value;
  }

  anchor(now) {
    if (!this.enabled) return;
    this.frameSim = now;
    this.frameAudio = this.ctx.currentTime + LATENCY;
  }

  when(simTime) {
    return Math.max(this.ctx.currentTime + 0.002, this.frameAudio + (simTime - this.frameSim) / 1e6);
  }

  // One event. A drum note goes to the drum voice that owns it and stops
  // there; everything else is offered to every player that asked for its
  // source. An event carries a `bus` only when it came off a note bus, which
  // is what tells the two sources apart.
  midi(event) {
    if (!this.enabled) return;
    const isNote = event.type === NOTE_ON || event.type === NOTE_OFF;
    // A drum has no note off: it rings for as long as its kit says it does.
    const strike = (voice) => {
      if (event.type === NOTE_ON && event.d2 > 0) {
        voice?.play(pieceOf(event.d1), { velocity: event.d2, note: event.d1, at: this.when(event.t) });
      }
    };
    if (isNote) {
      const voice = this.drumVoiceFor(event);
      if (voice) { strike(voice); return; }
    }
    const wanted = this.players.filter((player) => player.wants(event));
    // A drum note is never handed to a player, even when the player is what
    // asked for it: a pitched voice is the wrong instrument for a kick drum
    // however it is enveloped, and two players listening to one bus are not a
    // reason to hit the drum twice.
    if (isNote && event.channel === DRUM_CHANNEL) {
      if (wanted.length) strike(this.otherDrumFor(event));
      return;
    }
    for (const player of wanted) player.handle(event);
  }

  allOff(at = this.ctx?.currentTime ?? 0) {
    for (const player of this.players) player.allOff(at);
  }

  // A patch whose shape changed takes its nodes with it, and a node handed
  // over mid-note publishes its note off outside a pass - where nothing is
  // reading the buses. Rather than leave a bus player droning for ever on a
  // note whose owner no longer exists, the app says when it has replaced the
  // patch and everything sounding is let go.
  panic() {
    if (this.enabled) this.allOff(this.ctx.currentTime);
  }

  // --- saving what was set up ----------------------------------------------

  toJSON() {
    return {
      volume: this.volume,
      clicks: this.clicks,
      clickVolume: this.clickVolume,
      gateSources: this.gateSources.map(({ kind, index }) => ({ kind, index })),
      players: this.players.map((player) => player.toJSON()),
      drums: Object.fromEntries([...this.drums].map(([key, voice]) => [key, voice.toJSON()])),
    };
  }

  // A monitor setup is worth keeping across a reload: it is a page of choices
  // about a patch, and losing it on every refresh is the same annoyance as
  // losing the patch. Anything unreadable is ignored rather than thrown -
  // this is a convenience, not something to fail the app over.
  restore(saved) {
    if (!saved || typeof saved !== 'object') return;
    if (typeof saved.volume === 'number') this.setVolume(clamp(saved.volume));
    if (typeof saved.clicks === 'boolean') this.clicks = saved.clicks;
    if (typeof saved.clickVolume === 'number') this.setClickVolume(clamp(saved.clickVolume));
    if (Array.isArray(saved.gateSources)) {
      // A setup saved before the gate listener could be pointed anywhere has
      // no list at all, and the default is what it was doing.
      const sources = saved.gateSources.map((source) => gateSource(source)).filter(Boolean);
      this.gateSources = sources.slice(0, MAX_GATE_SOURCES);
    }
    if (saved.drums && typeof saved.drums === 'object') {
      for (const [key, config] of Object.entries(saved.drums)) {
        if (!config || typeof config !== 'object') continue;
        const voice = this.drumVoice(key, { label: key === OTHER_DRUMS ? 'anything else on channel 10' : key });
        voice.setKit(config.kit);
        if (typeof config.volume === 'number') voice.setVolume(clamp(config.volume));
        voice.setOn(config.on !== false);
      }
    }
    if (!Array.isArray(saved.players) || !saved.players.length) return;
    for (const player of [...this.players]) this.removePlayer(player.id);
    for (const config of saved.players.slice(0, MAX_PLAYERS)) {
      this.addPlayer({
        source: config.source,
        bus: Number(config.bus) || 0,
        wave: config.wave,
        volume: typeof config.volume === 'number' ? clamp(config.volume) : 1,
      });
    }
  }

  // --- the gates -----------------------------------------------------------

  addGateSource(config) {
    if (this.gateSources.length >= MAX_GATE_SOURCES) return null;
    const source = gateSource(config);
    if (!source) return null;
    this.gateSources.push(source);
    return source;
  }

  setGateSource(id, config) {
    const index = this.gateSources.findIndex((source) => source.id === id);
    const source = gateSource(config);
    if (index < 0 || !source) return;
    this.gateSources[index] = { ...source, id };
  }

  removeGateSource(id) {
    this.gateSources = this.gateSources.filter((source) => source.id !== id);
  }

  // Every edge this pass, once per pass rather than once per frame: a trigger
  // on this machine is high for a pass or two and an animation frame is
  // sixteen of them, so anything looking per frame hears one trigger in eight.
  //
  // Two things come out of it. A gate bus a drum sequencer's lane writes is a
  // *drum*, played on that sequencer's kit - which is how a `DrumSeqGate`,
  // which sends no MIDI at all and has no note numbers in it, is heard as a
  // kit rather than as eight identical beeps. Everything the gate listener has
  // been pointed at is a *click*, pitched by which jack or which bus it was.
  edges() {
    const module = this.module;
    let jackOut = 0;
    let jackAny = 0;
    for (let j = 0; j < module.jackSources.length; j++) {
      const mode = module.jackMode(j);
      if (mode === 2 && module.jackOutput(j)) { jackOut |= 1 << j; jackAny |= 1 << j; }
      else if (mode === 1 && module.jackInput(j)) jackAny |= 1 << j;
    }
    const gate = module.levels?.gate ?? 0;
    const rising = {
      jacksOut: jackOut & ~this.lastJackOut,
      jacks: jackAny & ~this.lastJackAny,
      buses: gate & ~this.lastGate,
    };
    this.lastJackOut = jackOut;
    this.lastJackAny = jackAny;
    this.lastGate = gate;
    if (!this.enabled) return;
    const at = this.when(module.now);

    if (rising.buses && this.gateRoute.size) {
      for (const [bus, { key, piece }] of this.gateRoute) {
        if (!((rising.buses >> bus) & 1)) continue;
        // A gate has no velocity: every hit is the same weight, which is the
        // whole reason the gate variant carries an accent lane instead.
        this.drums.get(key)?.play(piece, { velocity: 100, at });
      }
    }

    if (!this.clicks) return;
    for (const { kind, index } of gateHits(this.gateSources, rising)) this.click(kind, index, at);
  }

  // A blip, pitched by what fired: the jacks high and bright where they always
  // were, the buses below them, so "which of these am I hearing" survives
  // having both in the list at once.
  click(kind, index, at) {
    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();
    osc.type = 'sine';
    osc.frequency.value = kind === 'jack' ? 1500 + index * 250 : 800 + index * 130;
    gain.gain.setValueAtTime(0.2, at);
    gain.gain.exponentialRampToValueAtTime(0.001, at + 0.02);
    osc.connect(gain); gain.connect(this.clickGain);
    osc.start(at); osc.stop(at + 0.03);
  }
}

let gateSeq = 0;
const gateId = () => `g${++gateSeq}`;

// A gate source as this file will keep it: one of the three kinds, with an
// index that is in range for the kind. Anything else - from an old saved
// setup, or from a selector that has been given a value it should not have -
// is refused rather than kept as something that can never fire.
function gateSource(config) {
  if (!config || typeof config !== 'object') return null;
  const index = Number(config.index) || 0;
  if (config.kind === 'jacks') return { id: gateId(), kind: 'jacks', index: 0 };
  if (config.kind === 'jack' && index >= 0 && index < P.GPIO_N) return { id: gateId(), kind: 'jack', index };
  if (config.kind === 'bus' && index >= 0 && index < P.N_GATE_BUS) return { id: gateId(), kind: 'bus', index };
  return null;
}

const clamp = (value) => Math.max(0, Math.min(1, value));
