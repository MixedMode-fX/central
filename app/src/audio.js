// Hearing the patch.
//
// None of this is firmware. It is a small Web Audio synth standing in for
// whatever would be downstream of the module: it plays what `IMidiOut::send()`
// emits and clicks when an output jack goes high. A patch you can only read is
// a patch you cannot tell is wrong, and the module has no audio path of its
// own to borrow - so the page provides the ears.
//
// Events are scheduled on the audio clock at the *simulated* time they
// happened, anchored once per animation frame, so an arpeggio at 10 Hz sounds
// like one even though the passes that produced it ran in a burst.

const NOTE_OFF = 0x80, NOTE_ON = 0x90, CONTROL_CHANGE = 0xb0, PITCH_BEND = 0xe0;
const SUSTAIN = 64, ALL_SOUND_OFF = 120, ALL_NOTES_OFF = 123;
const DRUM_CHANNEL = 10;
const MAX_VOICES = 32;
const LATENCY = 0.03;

export class Listener {
  constructor(module) {
    this.module = module;
    this.ctx = null;
    this.master = null;
    this.voices = new Map();
    this.sustain = {};
    this.pending = new Set();
    this.bend = {};
    this.volume = 0.4;
    this.clicks = true;
    this.frameSim = 0;
    this.frameAudio = 0;
    this.lastJacks = 0;
    module.onMidi((event) => this.midi(event));
    module.onFrame((now) => this.anchor(now));
    module.onPass(() => this.jackEdges());
  }

  get enabled() { return this.ctx !== null && this.ctx.state === 'running'; }

  // Browsers only start audio from a user gesture, so this is a button and
  // never something the page does on its own.
  async toggle() {
    if (!this.ctx) {
      const Context = globalThis.AudioContext ?? globalThis.webkitAudioContext;
      if (!Context) throw new Error('this browser has no Web Audio');
      this.ctx = new Context();
      this.master = this.ctx.createGain();
      this.master.gain.value = this.volume;
      this.master.connect(this.ctx.destination);
    }
    if (this.ctx.state === 'running') {
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

  anchor(now) {
    if (!this.enabled) return;
    this.frameSim = now;
    this.frameAudio = this.ctx.currentTime + LATENCY;
  }

  when(simTime) {
    return Math.max(this.ctx.currentTime + 0.002, this.frameAudio + (simTime - this.frameSim) / 1e6);
  }

  midi(event) {
    if (!this.enabled) return;
    const { type, d1, d2, channel } = event;
    const at = this.when(event.t);
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
      osc.connect(gain); gain.connect(this.master);
      osc.start(at); osc.stop(at + 0.3);
      return;
    }

    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = 'sawtooth';
    osc.frequency.value = 440 * Math.pow(2, (note - 69) / 12);
    osc.detune.value = this.bend[channel] ?? 0;
    gain.gain.setValueAtTime(0, at);
    gain.gain.linearRampToValueAtTime(0.22 * velocity / 127, at + 0.005);
    osc.connect(gain); gain.connect(this.master);
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
      osc.connect(gain); gain.connect(this.master);
      osc.start(at); osc.stop(at + 0.03);
    }
  }
}
