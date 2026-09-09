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
// wrong" is a question about hearing them apart. The gate clicks are a third
// thing entirely - percussion made from jack edges, not notes - and they have
// their own level for the same reason: they are usually the loudest thing in
// the page and the one you want under the notes rather than over them.
//
// Events are scheduled on the audio clock at the *simulated* time they
// happened, anchored once per animation frame, so an arpeggio at 10 Hz sounds
// like one even though the passes that produced it ran in a burst.

const NOTE_OFF = 0x80, NOTE_ON = 0x90, CONTROL_CHANGE = 0xb0, PITCH_BEND = 0xe0;
const SUSTAIN = 64, ALL_SOUND_OFF = 120, ALL_NOTES_OFF = 123;
const DRUM_CHANNEL = 10;
const MAX_VOICES = 24;                 // per player
const MAX_PLAYERS = 6;
const LATENCY = 0.03;

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

  noteOn(channel, note, velocity, at) {
    const ctx = this.ctx;
    const key = channel * 128 + note;
    if (this.voices.has(key)) this.release(key, at);
    if (this.voices.size >= MAX_VOICES) this.release(this.voices.keys().next().value, at);

    // Channel 10 is drums by convention, and a drum sequencer that sounded
    // like a held sawtooth would be unreadable: a percussive voice that dies
    // on its own is what makes a pattern audible as a pattern.
    if (channel === DRUM_CHANNEL) {
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      const low = note < 40;
      const base = low ? 160 : 200 + (note - 40) * 40;
      osc.type = low ? 'sine' : (note % 2 ? 'square' : 'triangle');
      osc.frequency.setValueAtTime(base * 2, at);
      osc.frequency.exponentialRampToValueAtTime(Math.max(30, base / (low ? 4 : 1.5)), at + (low ? 0.12 : 0.05));
      gain.gain.setValueAtTime(0.35 * velocity / 127, at);
      gain.gain.exponentialRampToValueAtTime(0.001, at + (low ? 0.25 : 0.08));
      osc.connect(gain); gain.connect(this.gain);
      osc.start(at); osc.stop(at + 0.3);
      return;
    }

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

export class Listener {
  constructor(module) {
    this.module = module;
    this.ctx = null;
    this.master = null;
    this.clickGain = null;
    this.volume = 0.4;
    this.clicks = true;
    this.clickVolume = 0.5;
    this.players = [];
    this.frameSim = 0;
    this.frameAudio = 0;
    this.lastJacks = 0;
    module.onMidi((event) => this.midi(event));
    module.onNoteBus((event) => this.midi(event));
    module.onFrame((now) => this.anchor(now));
    module.onPass(() => this.jackEdges());
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

  // One event, offered to every player that asked for its source. An event
  // carries a `bus` only when it came off a note bus, which is what tells the
  // two sources apart.
  midi(event) {
    if (!this.enabled) return;
    for (const player of this.players) if (player.wants(event)) player.handle(event);
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
      players: this.players.map((player) => player.toJSON()),
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

  // --- the jacks -----------------------------------------------------------

  // A short blip per output jack on its rising edge, pitched by jack number,
  // which is what makes a clock division or a logic gate audible at all: those
  // patches send no MIDI.
  jackEdges() {
    let mask = 0;
    for (let j = 0; j < this.module.jackSources.length; j++) {
      if (this.module.jackMode(j) === 2 && this.module.jackOutput(j)) mask |= 1 << j;
    }
    const rising = mask & ~this.lastJacks;
    this.lastJacks = mask;
    if (!rising || !this.clicks || !this.enabled) return;
    const at = this.when(this.module.now);
    for (let j = 0; j < this.module.jackSources.length; j++) {
      if (!((rising >> j) & 1)) continue;
      const osc = this.ctx.createOscillator();
      const gain = this.ctx.createGain();
      osc.type = 'sine';
      osc.frequency.value = 1500 + j * 250;
      gain.gain.setValueAtTime(0.2, at);
      gain.gain.exponentialRampToValueAtTime(0.001, at + 0.02);
      osc.connect(gain); gain.connect(this.clickGain);
      osc.start(at); osc.stop(at + 0.03);
    }
  }
}

const clamp = (value) => Math.max(0, Math.min(1, value));
