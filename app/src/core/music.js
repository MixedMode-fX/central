// Notes, scales and chords in words: what a pitch is called, what a degree
// resolves to, and how a key spells its own notes.

import * as P from '../protocol/generated.js';

// Pitch classes, sharp and flat. The firmware stores a pitch class and has no
// opinion about spelling; a key does (`keySpelling`).
export const PITCH_CLASSES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export const PITCH_CLASSES_FLAT = ['C', 'D♭', 'D', 'E♭', 'E', 'F', 'G♭', 'G', 'A♭', 'A', 'B♭', 'B'];

export function noteName(pitch) {
  if (pitch < 0 || pitch > 127) return '—';
  return `${PITCH_CLASSES[pitch % 12]}${Math.floor(pitch / 12) - 1}`;
}

// Mirrors midi/scale.h so a displayed pitch is the one the module will play.
function scaleIntervals(mask) {
  const bits = (mask & 0x0fff) || 0x0fff;
  const out = [];
  for (let i = 0; i < 12; i++) if (bits & (1 << i)) out.push(i);
  return out;
}

export function degreeToSemitone(degree, mask) {
  const intervals = scaleIntervals(mask);
  const n = intervals.length;
  const octave = Math.floor(degree / n);
  let index = degree % n;
  if (index < 0) index += n;
  return octave * 12 + intervals[index];
}

// The quality of a triad, read off the pitch classes the firmware reports for
// it. Nothing in Harmony knows what a chord quality is - the stack is in
// scale steps, so the key decides - and this is the one place the app names
// what came out, to put an "m" on the label.
export function triadQuality(set, rootPc) {
  const steps = [];
  for (let pc = 0; pc < 12; pc++) if (set & (1 << pc)) steps.push((pc - rootPc + 12) % 12);
  steps.sort((a, b) => a - b);
  if (steps.length < 3) return 'other';
  const [, third, fifth] = steps;
  if (third === 3 && fifth === 6) return 'dim';
  if (third === 3 && fifth === 7) return 'min';
  if (third === 4 && fifth === 8) return 'aug';
  if (third === 4 && fifth === 7) return 'maj';
  return 'other';
}

// --- spelling a key ----------------------------------------------------------
//
// A pitch class is a number and a number has no spelling, so a list of sharps
// is right about half the time. In C minor that writes the third degree "D#",
// and a reader looking at a chord called D# where E flat belongs does not
// conclude that the app is bad at spelling - they conclude the degree is
// wrong, because D is the second and this thing is sitting on the third.
//
// The rule is the one a musician uses: a seven-note scale uses each letter
// once, in order, so the letter of degree i is the tonic's letter plus i and
// the accidental is whatever gets that letter to the pitch.

const LETTERS = ['C', 'D', 'E', 'F', 'G', 'A', 'B'];
const LETTER_PC = [0, 2, 4, 5, 7, 9, 11];
const ACCIDENTALS = { '-2': '♭♭', '-1': '♭', 0: '', 1: '#', 2: '×' };

// Semitones from a letter's natural pitch to `pc`, as a signed accidental in
// -6..5 rather than a distance: B to C is one up, not eleven.
const accidentalTo = (pc, letter) => ((pc - LETTER_PC[letter] + 18) % 12) - 6;

// The scale spelled from one candidate tonic letter, or null if any degree
// would need more than a double accidental. Scored by how many accidentals it
// costs, which is the whole of why C minor is not spelled from D.
function spellFrom(pcs, letter) {
  const names = [];
  let cost = 0;
  for (let i = 0; i < pcs.length; i++) {
    const l = (letter + i) % 7;
    const a = accidentalTo(pcs[i], l);
    if (a < -2 || a > 2) return null;
    cost += Math.abs(a);
    names.push(LETTERS[l] + ACCIDENTALS[a]);
  }
  return { names, cost };
}

// How to spell every pitch class of a key: a 12-entry table, indexed by pitch
// class, of the name that key gives it. Scales that are not seven notes have
// no letter-per-degree to follow, so they fall back to one flat-or-sharp
// decision for the whole key, taken from where the tonic sits on the circle
// of fifths: the six keys counter-clockwise of C are the flat ones.
export function keySpelling(root, mask) {
  const tonic = ((root % 12) + 12) % 12;
  const pcs = [];
  for (let step = 0; step < 12; step++) if ((mask >> step) & 1) pcs.push((tonic + step) % 12);

  const table = new Array(12).fill(null);
  if (pcs.length === 7) {
    let best = null;
    let bestAccidental = 0;
    for (let letter = 0; letter < 7; letter++) {
      // Only letters the tonic can actually be: C is never spelled from A.
      const a = accidentalTo(tonic, letter);
      if (a < -1 || a > 1) continue;
      const tried = spellFrom(pcs, letter);
      if (!tried) continue;
      // Fewest accidentals wins, and a tie goes to the flat spelling: E flat
      // minor and D sharp minor cost six each, and only one is ever written.
      if (best === null || tried.cost < best.cost
          || (tried.cost === best.cost && a < bestAccidental)) {
        best = tried;
        bestAccidental = a;
      }
    }
    if (best) for (let i = 0; i < pcs.length; i++) table[pcs[i]] ??= best.names[i];
  }

  const flat = ((tonic * 7) % 12) >= 6;
  for (let pc = 0; pc < 12; pc++) table[pc] ??= (flat ? PITCH_CLASSES_FLAT : PITCH_CLASSES)[pc];
  return table;
}

// The 12-bit mask of a key. The names live in protocol/names.js; this is the
// arithmetic on them the pictures need.
export const chromatic = (mask) => (mask & 0xfff) === 0xfff;
export const hasLeadingTone = (mask) => Boolean((mask >> 11) & 1);
export const inScale = (mask, step) => Boolean((mask >> (((step % 12) + 12) % 12)) & 1);

// The key's default register, as a MIDI note (src/midi/global_key.h).
export const registerNote = (octave, root) => Math.min(127, octave * 12 + root);
export const DEFAULT_KEY_OCTAVE = P.KEY_DEFAULT_OCTAVE;
export const MAX_KEY_OCTAVE = P.KEY_MAX_OCTAVE;
