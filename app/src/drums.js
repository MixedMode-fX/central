// The drum voice, and the kits it can be pointed at.
//
// Everything else this page makes a noise with is a synth voice standing in
// for whatever would be downstream of the module, and a drum sequencer used to
// get the same treatment with one line of special case: an oscillator pitched
// by note number, swept down, gone in 80 ms. That is enough to hear *a
// pattern* and nowhere near enough to hear *drums* - a kick and a snare came
// out as the same beep two tones apart, so the one question a drum grid is
// edited to answer, whether the backbeat lands where it should against the
// hats, was the one question it could not answer.
//
// So: a real drum voice, and a few kits to choose between.
//
// **The kits are synthesised, not sampled**, and that is a deliberate choice
// rather than a shortcut. The app is ES modules with no build step, and the
// build that matters - `emulator/dist/index.html`, the CI artifact - is *one
// file* that opens from a download with nothing serving it. A sample set is
// megabytes of audio that a `file://` page cannot fetch and that would have to
// be base64 in the page instead, for a preview whose whole job is to say which
// lane just fired. The classic machines were synthesisers anyway: the 808 hat
// is six square oscillators through a high-pass, not a recording, and it is
// cheaper to build it than to store it.
//
// A drum here is at most four ingredients, and every kit below is those four
// with different numbers:
//
//   * **body** - a pitched oscillator with a pitch envelope. The tone: a
//     kick's drop, a tom's fall, a rimshot's ping.
//   * **noise** - a band of filtered noise. The skin of a snare, the sand of a
//     shaker, the sizzle of a cymbal. Repeated in quick bursts it is a clap,
//     which is what a clap physically is.
//   * **metal** - a cluster of square oscillators through a high-pass. The
//     machine hat and the cowbell, which are not noise at all: the 808's six
//     are 205.3, 304.4, 369.6, 522.7, 540 and 800 Hz, and the cowbell is the
//     last two on their own.
//   * **click** - the transient. Without it a kick on a laptop speaker is
//     inaudible rather than quiet.
//
// Nothing in here is firmware, and nothing in here is on the module. It is the
// page's ears, the same as `audio.js` around it.

import * as P from './protocol.js';

// --- what a lane is, and what a note is -------------------------------------

// The note a `DrumSeqMidi` lane sends when its note number is left at zero.
// This mirrors `GM_DEFAULT_NOTE` in `src/algorithm/sequencer/drum_sequencer.cpp`
// and is checked against it by `app/test/app.test.mjs`, because it is the one
// thing here that is a fact about the firmware rather than a choice about
// sound: it is what makes a `DrumSeqGate` lane - which carries no note number
// at all, only a gate - audible as the drum the firmware means by that lane.
export const LANE_NOTES = [36, 38, 42, 46, 41, 45, 49, 51];

export const PIECES = [
  'kick', 'snare', 'rim', 'clap', 'hatClosed', 'hatPedal', 'hatOpen',
  'tomLow', 'tomMid', 'tomHigh', 'crash', 'ride', 'cowbell', 'clave', 'shaker', 'perc',
];

export const PIECE_LABELS = {
  kick: 'kick', snare: 'snare', rim: 'rim', clap: 'clap',
  hatClosed: 'closed hat', hatPedal: 'pedal hat', hatOpen: 'open hat',
  tomLow: 'low tom', tomMid: 'mid tom', tomHigh: 'high tom',
  crash: 'crash', ride: 'ride', cowbell: 'cowbell', clave: 'clave', shaker: 'shaker',
  perc: 'percussion',
};

// General MIDI percussion, 35..81. A drum sequencer sends note numbers and
// this is what the world agrees they mean, so a lane left at its default plays
// the drum its default *names* - and a lane moved to 39 plays a clap in every
// kit rather than "the same beep, one semitone up".
const GM = {
  35: 'kick', 36: 'kick', 37: 'rim', 38: 'snare', 39: 'clap', 40: 'snare',
  41: 'tomLow', 42: 'hatClosed', 43: 'tomLow', 44: 'hatPedal', 45: 'tomMid',
  46: 'hatOpen', 47: 'tomMid', 48: 'tomHigh', 49: 'crash', 50: 'tomHigh',
  51: 'ride', 52: 'crash', 53: 'ride', 54: 'shaker', 55: 'crash', 56: 'cowbell',
  57: 'crash', 58: 'shaker', 59: 'ride', 60: 'tomHigh', 61: 'tomMid',
  62: 'tomHigh', 63: 'tomMid', 64: 'tomLow', 65: 'tomHigh', 66: 'tomMid',
  67: 'cowbell', 68: 'cowbell', 69: 'shaker', 70: 'shaker', 71: 'shaker',
  72: 'shaker', 73: 'shaker', 74: 'shaker', 75: 'clave', 76: 'clave',
  77: 'clave', 78: 'shaker', 79: 'shaker', 80: 'clave', 81: 'clave',
};

// A note outside the GM map is not nothing: it is a drum sequencer somebody
// has pointed at a sampler of their own. It gets the tuned percussion voice,
// pitched by the note, which is at least honest about being a stand-in.
export function pieceOf(note) {
  return GM[note] ?? 'perc';
}

// --- the recipes ------------------------------------------------------------

// `acoustic` is spelled out in full and the other three are written as
// differences from it, because a kit is *the same drums, made differently* -
// and a table of four full kits is four places to fix one wrong decay. A null
// removes an ingredient: an 808 hat has no noise in it at all.
const ACOUSTIC = {
  kick:      { body: { wave: 'sine', from: 150, to: 48, sweep: 0.06, decay: 0.42 },
               click: { freq: 1400, decay: 0.012, gain: 0.28 } },
  snare:     { body: { wave: 'triangle', from: 250, to: 180, sweep: 0.05, decay: 0.11, gain: 0.45 },
               noise: { type: 'bandpass', freq: 1800, q: 0.6, decay: 0.19, gain: 0.7 },
               click: { freq: 3000, decay: 0.008, gain: 0.2 } },
  rim:       { body: { wave: 'square', from: 1700, decay: 0.03, gain: 0.35 },
               noise: { type: 'highpass', freq: 3000, decay: 0.03, gain: 0.25 } },
  clap:      { noise: { type: 'bandpass', freq: 1200, q: 1.1, decay: 0.21, gain: 0.75,
                        bursts: 3, spacing: 0.011, burstDecay: 0.02 } },
  hatClosed: { noise: { type: 'highpass', freq: 7000, decay: 0.045, gain: 0.45 } },
  hatPedal:  { noise: { type: 'highpass', freq: 6000, decay: 0.08, gain: 0.4 } },
  hatOpen:   { noise: { type: 'highpass', freq: 6500, decay: 0.34, gain: 0.4 } },
  tomLow:    { body: { wave: 'sine', from: 130, to: 82, sweep: 0.14, decay: 0.42, gain: 0.8 },
               noise: { type: 'bandpass', freq: 500, q: 0.8, decay: 0.09, gain: 0.15 } },
  tomMid:    { body: { wave: 'sine', from: 185, to: 115, sweep: 0.12, decay: 0.36, gain: 0.8 },
               noise: { type: 'bandpass', freq: 700, q: 0.8, decay: 0.08, gain: 0.15 } },
  tomHigh:   { body: { wave: 'sine', from: 250, to: 155, sweep: 0.1, decay: 0.3, gain: 0.8 },
               noise: { type: 'bandpass', freq: 950, q: 0.8, decay: 0.07, gain: 0.15 } },
  crash:     { noise: { type: 'highpass', freq: 5000, decay: 1.3, gain: 0.42 },
               metal: { freqs: [523, 741, 987], highpass: 5000, decay: 0.9, gain: 0.06 } },
  ride:      { noise: { type: 'highpass', freq: 7500, decay: 0.85, gain: 0.22 },
               body: { wave: 'triangle', from: 900, decay: 0.24, gain: 0.18 } },
  cowbell:   { metal: { freqs: [540, 800], highpass: 450, decay: 0.35, gain: 0.32 } },
  clave:     { body: { wave: 'sine', from: 2400, decay: 0.045, gain: 0.5 } },
  shaker:    { noise: { type: 'highpass', freq: 9000, decay: 0.09, gain: 0.35 } },
  // The fallback, and the only tuned voice: `tuned` is what tells `hit()` to
  // move it by note number instead of playing one fixed drum.
  perc:      { tuned: true,
               body: { wave: 'triangle', from: 300, to: 200, sweep: 0.05, decay: 0.18, gain: 0.5 },
               click: { freq: 4000, decay: 0.006, gain: 0.15 } },
};

// The six squares every machine cymbal in here is built from.
const METAL_SIX = [205.3, 304.4, 369.6, 522.7, 540, 800];

const EIGHT_OH_EIGHT = {
  // The long sine kick the machine is known for: barely any sweep, and a
  // decay measured in seconds rather than in milliseconds.
  kick:      { body: { wave: 'sine', from: 110, to: 44, sweep: 0.1, decay: 1.05 },
               click: { freq: 1200, decay: 0.008, gain: 0.14 } },
  snare:     { body: { wave: 'triangle', from: 200, to: 178, sweep: 0.04, decay: 0.09, gain: 0.4 },
               noise: { type: 'highpass', freq: 2000, decay: 0.13, gain: 0.5 },
               click: null },
  rim:       { body: { wave: 'square', from: 1600, decay: 0.02, gain: 0.45 }, noise: null },
  clap:      { noise: { type: 'bandpass', freq: 1000, q: 1.4, decay: 0.24, gain: 0.7,
                        bursts: 4, spacing: 0.01, burstDecay: 0.018 } },
  hatClosed: { noise: null, metal: { freqs: METAL_SIX, highpass: 7200, decay: 0.05, gain: 0.3 } },
  hatPedal:  { noise: null, metal: { freqs: METAL_SIX, highpass: 7200, decay: 0.1, gain: 0.28 } },
  hatOpen:   { noise: null, metal: { freqs: METAL_SIX, highpass: 7200, decay: 0.42, gain: 0.26 } },
  tomLow:    { body: { from: 95, to: 62, sweep: 0.18, decay: 0.6 }, noise: null },
  tomMid:    { body: { from: 140, to: 90, sweep: 0.16, decay: 0.5 }, noise: null },
  tomHigh:   { body: { from: 200, to: 128, sweep: 0.14, decay: 0.42 }, noise: null },
  crash:     { noise: { type: 'highpass', freq: 8000, decay: 1.1, gain: 0.12 },
               metal: { freqs: METAL_SIX, highpass: 4000, decay: 1.5, gain: 0.24 } },
  ride:      { body: null, noise: { type: 'highpass', freq: 8000, decay: 0.5, gain: 0.08 },
               metal: { freqs: METAL_SIX, highpass: 6000, decay: 0.8, gain: 0.2 } },
  cowbell:   { metal: { freqs: [540, 800], highpass: 300, decay: 0.38, gain: 0.36 } },
  clave:     { body: { from: 2500, decay: 0.05 } },
  shaker:    { noise: { freq: 10000, decay: 0.06, gain: 0.3 } },
  perc:      { body: { wave: 'sine', sweep: 0.08, decay: 0.3 }, click: null },
};

const NINE_OH_NINE = {
  // Where the 808 drops slowly from low, this one snaps down from high and
  // leads with a click - which is the whole difference on a small speaker.
  kick:      { body: { wave: 'sine', from: 210, to: 50, sweep: 0.032, decay: 0.5 },
               click: { freq: 1800, decay: 0.014, gain: 0.42 } },
  // Mostly noise, with just enough tone under it to have a pitch.
  snare:     { body: { wave: 'triangle', from: 330, to: 190, sweep: 0.035, decay: 0.055, gain: 0.28 },
               noise: { type: 'highpass', freq: 1500, decay: 0.17, gain: 0.8 },
               click: { freq: 4000, decay: 0.006, gain: 0.22 } },
  rim:       { body: { wave: 'square', from: 1750, decay: 0.025, gain: 0.4 },
               noise: { type: 'highpass', freq: 3500, decay: 0.02, gain: 0.3 } },
  clap:      { noise: { type: 'bandpass', freq: 1100, q: 1.2, decay: 0.3, gain: 0.8,
                        bursts: 3, spacing: 0.009, burstDecay: 0.016 } },
  hatClosed: { noise: { type: 'highpass', freq: 8500, decay: 0.05, gain: 0.32 },
               metal: { freqs: METAL_SIX, highpass: 9000, decay: 0.06, gain: 0.14 } },
  hatPedal:  { noise: { type: 'highpass', freq: 8000, decay: 0.09, gain: 0.3 },
               metal: { freqs: METAL_SIX, highpass: 9000, decay: 0.1, gain: 0.12 } },
  hatOpen:   { noise: { type: 'highpass', freq: 8000, decay: 0.32, gain: 0.28 },
               metal: { freqs: METAL_SIX, highpass: 9000, decay: 0.34, gain: 0.12 } },
  tomLow:    { body: { from: 120, to: 66, sweep: 0.11, decay: 0.36 },
               noise: { freq: 400, decay: 0.05, gain: 0.1 } },
  tomMid:    { body: { from: 170, to: 96, sweep: 0.1, decay: 0.32 },
               noise: { freq: 600, decay: 0.05, gain: 0.1 } },
  tomHigh:   { body: { from: 235, to: 140, sweep: 0.09, decay: 0.28 },
               noise: { freq: 850, decay: 0.05, gain: 0.1 } },
  crash:     { noise: { type: 'highpass', freq: 6000, decay: 1.4, gain: 0.34 },
               metal: { freqs: METAL_SIX, highpass: 7000, decay: 1.2, gain: 0.1 } },
  ride:      { body: { wave: 'triangle', from: 1100, decay: 0.2, gain: 0.14 },
               noise: { type: 'highpass', freq: 9000, decay: 0.7, gain: 0.24 } },
  cowbell:   { metal: { freqs: [587, 845], highpass: 400, decay: 0.3, gain: 0.34 } },
  shaker:    { noise: { freq: 11000, decay: 0.07, gain: 0.32 } },
};

// Not a machine anybody made: the drum voice a modular rack builds out of an
// oscillator, a filter and two envelopes. Squares and sawtooth where the
// others use sine, sweeps long enough to hear as a pitch bend, and a ring to
// everything - which is exactly what a patched-up drum sounds like, and what
// makes it read as *this module's* drums rather than a machine's.
const SYNTH = {
  kick:      { body: { wave: 'triangle', from: 240, to: 36, sweep: 0.13, decay: 0.6 },
               click: { freq: 2200, decay: 0.01, gain: 0.3 } },
  snare:     { body: { wave: 'square', from: 320, to: 165, sweep: 0.07, decay: 0.13, gain: 0.3 },
               noise: { type: 'bandpass', freq: 2400, q: 0.9, decay: 0.13, gain: 0.55 },
               click: { freq: 5000, decay: 0.005, gain: 0.2 } },
  rim:       { body: { wave: 'square', from: 2100, to: 1200, sweep: 0.02, decay: 0.035, gain: 0.35 },
               noise: null },
  clap:      { noise: { type: 'bandpass', freq: 1600, q: 2.2, decay: 0.16, gain: 0.6,
                        bursts: 3, spacing: 0.013, burstDecay: 0.02 } },
  hatClosed: { noise: { type: 'highpass', freq: 6500, decay: 0.04, gain: 0.3 },
               metal: { freqs: [3100, 4300], highpass: 5000, decay: 0.05, gain: 0.16 } },
  hatPedal:  { noise: { type: 'highpass', freq: 6000, decay: 0.08, gain: 0.28 },
               metal: { freqs: [3100, 4300], highpass: 5000, decay: 0.09, gain: 0.14 } },
  hatOpen:   { noise: { type: 'highpass', freq: 6000, decay: 0.3, gain: 0.26 },
               metal: { freqs: [3100, 4300], highpass: 5000, decay: 0.32, gain: 0.14 } },
  tomLow:    { body: { wave: 'sawtooth', from: 150, to: 70, sweep: 0.2, decay: 0.4, gain: 0.55 },
               noise: null },
  tomMid:    { body: { wave: 'sawtooth', from: 210, to: 100, sweep: 0.18, decay: 0.34, gain: 0.55 },
               noise: null },
  tomHigh:   { body: { wave: 'sawtooth', from: 290, to: 140, sweep: 0.16, decay: 0.3, gain: 0.55 },
               noise: null },
  crash:     { noise: { type: 'highpass', freq: 4500, decay: 1.1, gain: 0.3 },
               metal: { freqs: [3100, 4300, 5600], highpass: 4000, decay: 1.2, gain: 0.14 } },
  ride:      { body: { wave: 'square', from: 1300, decay: 0.18, gain: 0.12 },
               noise: { type: 'highpass', freq: 7000, decay: 0.6, gain: 0.18 } },
  cowbell:   { metal: { freqs: [660, 990], highpass: 500, decay: 0.28, gain: 0.3 } },
  clave:     { body: { wave: 'square', from: 2200, decay: 0.035, gain: 0.4 } },
  shaker:    { noise: { type: 'highpass', freq: 8000, decay: 0.11, gain: 0.3 } },
  perc:      { body: { wave: 'square', from: 340, to: 160, sweep: 0.07, decay: 0.22, gain: 0.4 } },
};

export const KITS = [
  { id: 'acoustic', label: 'acoustic',
    about: 'a kit in a room: tuned shells, a snare with skin on it, hats made of noise' },
  { id: '808', label: '808',
    about: 'the long sine kick, the six-oscillator metal hat, the cowbell' },
  { id: '909', label: '909',
    about: 'a kick that snaps down from high with a click on it, and a noisy snare' },
  { id: 'synth', label: 'drum synth',
    about: 'no machine in particular: squares, long sweeps and a ring — a rack building drums' },
];

export const DEFAULT_KIT = '909';

const OVERRIDES = { acoustic: {}, 808: EIGHT_OH_EIGHT, 909: NINE_OH_NINE, synth: SYNTH };

// One kit, resolved once: the base with its differences folded in, so playing
// a note is a lookup rather than a merge.
function resolve(overrides) {
  const kit = {};
  for (const piece of PIECES) {
    const base = ACOUSTIC[piece] ?? ACOUSTIC.perc;
    const over = overrides[piece] ?? {};
    const spec = { tuned: base.tuned ?? false };
    for (const part of ['body', 'noise', 'metal', 'click']) {
      if (part in over && over[part] === null) continue;         // this kit does without it
      if (!base[part] && !over[part]) continue;
      spec[part] = { ...(base[part] ?? {}), ...(over[part] ?? {}) };
    }
    kit[piece] = spec;
  }
  return kit;
}

const RESOLVED = Object.fromEntries(Object.entries(OVERRIDES).map(([id, over]) => [id, resolve(over)]));

export function kitLabel(id) {
  return KITS.find((kit) => kit.id === id)?.label ?? id;
}

// The recipe a kit plays for a piece. Exported because it is the part worth
// testing with no audio hardware in the room: that every kit can play every
// piece, and that none of them resolved to silence.
export function voiceSpec(kit, piece) {
  return (RESOLVED[kit] ?? RESOLVED[DEFAULT_KIT])[piece] ?? RESOLVED[DEFAULT_KIT].perc;
}

// --- making the sound -------------------------------------------------------

const ATTACK = 0.001;      // not zero: a step from silence is a click of its own
const FLOOR = 0.0005;      // an exponential ramp cannot reach zero
const TAIL = 0.05;         // let a node finish before it is stopped

// One second of white noise, made once per audio context and looped. Every
// noise ingredient in every kit reads this same buffer.
const NOISE = new WeakMap();
function noiseBuffer(ctx) {
  let buffer = NOISE.get(ctx);
  if (buffer) return buffer;
  buffer = ctx.createBuffer(1, Math.floor(ctx.sampleRate), ctx.sampleRate);
  const data = buffer.getChannelData(0);
  for (let i = 0; i < data.length; i++) data[i] = Math.random() * 2 - 1;
  NOISE.set(ctx, buffer);
  return buffer;
}

function envelope(param, peak, at, decay, hold = 0) {
  const top = Math.max(FLOOR * 2, peak);
  const end = at + ATTACK + hold + Math.max(0.005, decay);
  param.setValueAtTime(0, at);
  param.linearRampToValueAtTime(top, at + ATTACK);
  if (hold) param.setValueAtTime(top, at + ATTACK + hold);
  param.exponentialRampToValueAtTime(FLOOR, end);
  param.setValueAtTime(0, end + 0.001);
  return end;
}

function playBody(ctx, out, spec, at, level, ratio) {
  const osc = ctx.createOscillator();
  const gain = ctx.createGain();
  const from = (spec.from ?? 200) * ratio;
  const to = (spec.to ?? spec.from ?? 200) * ratio;
  osc.type = spec.wave ?? 'sine';
  osc.frequency.setValueAtTime(from, at);
  if (to !== from) osc.frequency.exponentialRampToValueAtTime(Math.max(20, to), at + (spec.sweep ?? 0.05));
  const end = envelope(gain.gain, (spec.gain ?? 1) * level, at, spec.decay ?? 0.2, spec.hold ?? 0);
  osc.connect(gain).connect(out);
  osc.start(at);
  osc.stop(end + TAIL);
}

// Noise through one filter. `bursts` is a clap: three or four of them a few
// milliseconds apart, the last one longer, which is what a clap is.
function playNoise(ctx, out, spec, at, level, ratio) {
  const bursts = Math.max(1, spec.bursts ?? 1);
  for (let i = 0; i < bursts; i++) {
    const last = i === bursts - 1;
    const start = at + i * (spec.spacing ?? 0);
    const decay = last ? (spec.decay ?? 0.1) : (spec.burstDecay ?? 0.02);
    const source = ctx.createBufferSource();
    source.buffer = noiseBuffer(ctx);
    source.loop = true;
    const filter = ctx.createBiquadFilter();
    filter.type = spec.type ?? 'bandpass';
    filter.frequency.value = Math.min(ctx.sampleRate / 2 - 1, (spec.freq ?? 2000) * ratio);
    filter.Q.value = spec.q ?? 1;
    const gain = ctx.createGain();
    const end = envelope(gain.gain, (spec.gain ?? 0.5) * level * (last ? 1 : 0.8), start, decay, spec.hold ?? 0);
    source.connect(filter).connect(gain).connect(out);
    source.start(start);
    source.stop(end + TAIL);
  }
}

// A cluster of squares through a high-pass: the machine cymbal, which has no
// noise in it at all.
function playMetal(ctx, out, spec, at, level) {
  const filter = ctx.createBiquadFilter();
  filter.type = 'highpass';
  filter.frequency.value = Math.min(ctx.sampleRate / 2 - 1, spec.highpass ?? 6000);
  filter.Q.value = spec.q ?? 0.7;
  const gain = ctx.createGain();
  const end = envelope(gain.gain, (spec.gain ?? 0.25) * level, at, spec.decay ?? 0.3, spec.hold ?? 0);
  filter.connect(gain).connect(out);
  for (const frequency of spec.freqs ?? METAL_SIX) {
    const osc = ctx.createOscillator();
    osc.type = spec.wave ?? 'square';
    osc.frequency.value = frequency;
    osc.connect(filter);
    osc.start(at);
    osc.stop(end + TAIL);
  }
}

function playClick(ctx, out, spec, at, level) {
  const source = ctx.createBufferSource();
  source.buffer = noiseBuffer(ctx);
  source.loop = true;
  const filter = ctx.createBiquadFilter();
  filter.type = 'highpass';
  filter.frequency.value = Math.min(ctx.sampleRate / 2 - 1, spec.freq ?? 3000);
  const gain = ctx.createGain();
  const end = envelope(gain.gain, (spec.gain ?? 0.2) * level, at, spec.decay ?? 0.01);
  source.connect(filter).connect(gain).connect(out);
  source.start(at);
  source.stop(end + TAIL);
}

// One hit. `piece` is what to play, `note` only matters for the tuned
// percussion voice - the fallback for a note number General MIDI says nothing
// about - and `velocity` is the MIDI velocity, or 100 for a gate, which
// carries no such thing.
export function hit(ctx, destination, { kit = DEFAULT_KIT, piece = 'perc', note = null, velocity = 100, at = 0 } = {}) {
  const spec = voiceSpec(kit, piece);
  const level = Math.max(0.06, Math.min(1, velocity / 127));
  const ratio = spec.tuned && note !== null ? Math.pow(2, (note - 60) / 12) : 1;
  if (spec.body) playBody(ctx, destination, spec.body, at, level, ratio);
  if (spec.noise) playNoise(ctx, destination, spec.noise, at, level, ratio);
  if (spec.metal) playMetal(ctx, destination, spec.metal, at, level);
  if (spec.click) playClick(ctx, destination, spec.click, at, level);
}

// --- which drum sequencers a patch has --------------------------------------

// The drum sequencers in a patch, each with the buses it speaks on. This is
// what makes a kit and a level *per drum sequencer* possible at all: two drum
// machines in one patch are two instruments, and "which of these two is the
// one I can hear" is exactly the question a shared voice cannot answer.
//
// A node's identity is its index, the same identity the canvas uses to
// remember where a block was dragged to and the same one every addressed
// message carries. It is pure - patch in, list out, no audio and no DOM - so
// what it maps can be checked against the firmware's own registry in a test.
export function drumSources(device, patch) {
  const sources = [];
  if (!device || !patch) return sources;
  patch.nodes.forEach((node, index) => {
    const descriptor = device.byId?.get(node.algorithmId);
    if (!descriptor) return;
    const midi = descriptor.name === 'DrumSeqMidi';
    if (!midi && descriptor.name !== 'DrumSeqGate') return;
    const lanes = [];
    let bus = P.NO_BUS;
    if (midi) {
      // One note outlet for every lane: the note number says which drum, so
      // the lanes are told apart by what they play rather than by where.
      bus = node.outBus[0] ?? P.NO_BUS;
    } else {
      // One gate outlet per lane, and a gate carries no note number - so the
      // lane *is* the drum, by the firmware's own default note for it.
      for (let lane = 0; lane < P.DRUM_SEQ_LANES && lane < descriptor.nOut; lane++) {
        const laneBus = node.outBus[lane] ?? P.NO_BUS;
        if (laneBus === P.NO_BUS) continue;
        lanes.push({ lane, bus: laneBus, piece: pieceOf(LANE_NOTES[lane]) });
      }
    }
    sources.push({
      key: `node:${index}`,
      index,
      label: `${descriptor.name} ${index}`,
      kind: midi ? 'note' : 'gate',
      bus,
      lanes,
    });
  });
  return sources;
}
