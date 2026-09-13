// Names for the things the wire carries as numbers.
//
// `protocol.js` is generated from the firmware headers and carries the *values*
// - MidiPort bits, clock sources, takeover modes. It cannot carry the words a
// musician knows them by, because those are not in the headers. This file is
// where those words live, and every table here is **keyed by the generated
// enum's own identifier**, then checked against it at load: rename a MidiPort
// in the firmware and this throws on the first import rather than quietly
// labelling the wrong cable.
//
// Everything that *is* in the headers - an algorithm's name, its inlets and
// outlets, a parameter's name and its enum options - is read from the device
// and never appears here. This is only for the fields the protocol defines by
// enum rather than by description.

import * as P from './protocol.js';

function labelled(enumeration, labels, what) {
  const missing = Object.keys(enumeration).filter((key) => !(key in labels));
  if (missing.length) throw new Error(`${what}: no label for ${missing.join(', ')}`);
  const extra = Object.keys(labels).filter((key) => !(key in enumeration));
  if (extra.length) throw new Error(`${what}: ${extra.join(', ')} is not in the firmware enum`);
  return Object.entries(labels).map(([key, label]) => ({ key, label, value: enumeration[key] }));
}

// The MIDI endpoints, as a DAW lists them: the four USB device cables are
// numbered from 1 here and from 0 in the firmware, which is the one place the
// app deliberately renumbers something.
export const MIDI_PORTS = labelled(P.MidiPort, {
  mmMIDI_USB_0: 'USB 1',
  mmMIDI_USB_1: 'USB 2',
  mmMIDI_USB_2: 'USB 3',
  mmMIDI_USB_3: 'USB 4',
  mmMIDI_SERIAL_1: 'DIN 1',
  mmMIDI_SERIAL_2: 'DIN 2',
  mmMIDI_SERIAL_3: 'DIN 3',
  mmMIDI_HOST_1: 'USB host',
}, 'MidiPort');

// The control cable is reserved for the protocol, so it is never offered as a
// musical route or as a controller source: a patch that could take it away
// would leave reflashing as the only way back.
export const MUSICAL_PORTS = MIDI_PORTS.filter((p) => p.value !== P.MIDI_CONTROL_PORT);

// Every cable but the control port: what a binding made with no controller in
// the room listens on, because a patch cannot know which one it will arrive
// on. The control cable is never in it - a mapping that could take the
// protocol away from the app is a mapping that could lock the module out.
export const ALL_MUSICAL = MUSICAL_PORTS.reduce((mask, p) => mask | p.value, 0);

export function portNames(mask) {
  const found = MIDI_PORTS.filter((p) => (mask & p.value) !== 0).map((p) => p.label);
  return found.length ? found : [];
}

export const portMaskOf = (names) => MIDI_PORTS
  .filter((p) => names.includes(p.label))
  .reduce((mask, p) => mask | p.value, 0);

// The scales, by the names a musician uses. The *masks* are generated from
// midi/scale.h (P.ScaleMask), so only the words are here - and `labelled`
// fails the moment the firmware gains or renames one.
//
// Id 0 is not a scale but an unset byte, so it is left out of the list a key
// is chosen from - and there is no other list, because the key is the only
// place a scale is named (src/midi/global_key.h).
export const SCALES = labelled(P.ScaleId, {
  SCALE_NONE: '',
  SCALE_MAJOR: 'major',
  SCALE_NATURAL_MINOR: 'minor',
  SCALE_HARMONIC_MINOR: 'harmonic minor',
  SCALE_MELODIC_MINOR: 'melodic minor',
  SCALE_PENTATONIC_MAJOR: 'pentatonic major',
  SCALE_PENTATONIC_MINOR: 'pentatonic minor',
  SCALE_BLUES: 'blues',
  SCALE_DORIAN: 'dorian',
  SCALE_PHRYGIAN: 'phrygian',
  SCALE_LYDIAN: 'lydian',
  SCALE_MIXOLYDIAN: 'mixolydian',
  SCALE_LOCRIAN: 'locrian',
  SCALE_WHOLE_TONE: 'whole tone',
  SCALE_CHROMATIC: 'chromatic',
  SCALE_COUNT: '',
}, 'ScaleId').filter((s) => s.label);

// The scales a key can be in: kept as its own name because that is what the
// key tab reads, and every scale there is.
export const KEY_SCALES = SCALES;

// Pitch classes, for a root. Sharps rather than flats, because the firmware
// stores a pitch class and has no opinion about spelling.
export const PITCH_CLASSES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

// **The same twelve pitch classes, spelled the way the key spells them.**
//
// A pitch class is a number and a number has no spelling, so the list above
// picks sharps and is right about half the time. In C minor that writes the
// third degree "D#" and the sixth "G#", and a reader looking at a chord
// called D# where E flat belongs does not conclude that the app is bad at
// spelling - they conclude the degree is wrong, because D is the second and
// this thing is sitting on the third. The names are not decoration: they are
// what says which chord this is.
//
// The rule is the one a musician uses and not a preference for flats: a
// seven-note scale uses each letter once, in order, so the letter of degree i
// is the tonic's letter plus i and the accidental is whatever gets that
// letter to the pitch. That is what makes C minor E flat rather than D sharp,
// and C sharp minor E sharp rather than F.
// The same twelve written the other way round. A pitch class the key has no
// letter for - the five it does not play - is named from one of these two.
export const PITCH_CLASSES_FLAT =
  ['C', 'D♭', 'D', 'E♭', 'E', 'F', 'G♭', 'G', 'A♭', 'A', 'B♭', 'B'];

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
// class, of the name that key gives it.
//
// Scales that are not seven notes have no letter-per-degree to follow - a
// pentatonic skips two of them and the chromatic has all twelve - so they
// fall back to one flat-or-sharp decision for the whole key, taken from where
// the tonic sits on the circle of fifths: the six keys counter-clockwise of C
// are the flat ones.
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
      // minor and D sharp minor cost six each, and only one of them is ever
      // written down.
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

// The key's register, and the octave a node plays in when it names none of
// its own (src/midi/global_key.h).
export const DEFAULT_KEY_OCTAVE = P.KEY_DEFAULT_OCTAVE;
export const MAX_KEY_OCTAVE = P.KEY_MAX_OCTAVE;

// The id, for the one place that stores a ScaleId: the key in the globals.
export function scaleIdOf(name) {
  if (typeof name === 'number') return name & 0xff;
  const found = SCALES.find((s) => s.label === String(name).toLowerCase().replace(/[_-]+/g, ' ').trim());
  if (!found) {
    throw new Error(`unknown scale "${name}" (one of ${SCALES.map((s) => s.label).join(', ')})`);
  }
  return found.value;
}

export const scaleName = (id) => SCALES.find((s) => s.value === id)?.label ?? String(id);

// The 12-bit mask a scale id plays, from the firmware's own table. Anything
// the module does not know reads as chromatic, which is what it plays.
export function scaleMaskById(id) {
  const found = SCALES.find((s) => s.value === id);
  return found ? P.ScaleMask[found.key] : P.ScaleMask.SCALE_CHROMATIC;
}

export function scaleMaskOf(name) {
  const found = SCALES.find((s) => s.label === String(name).toLowerCase().replace(/[_-]+/g, ' ').trim());
  if (!found) {
    throw new Error(`unknown scale "${name}" `
                  + `(one of ${SCALES.map((s) => s.label).join(', ')}, or a 12-bit mask)`);
  }
  return P.ScaleMask[found.key];
}

// StepEngine::Direction. A class body, which the generator deliberately does
// not parse, so the order is spelled out and the comment says where from.
export const STEP_DIRECTIONS = ['forward', 'reverse', 'pingpong', 'random', 'brownian'];

// Metronome::Division and Metronome::Feel, spelled out for the same reason
// and in the same order - slowest first - as algorithm/clock/metronome.h. Both
// enums count from 1, so the index into these arrays is the stored byte minus
// one; a stored 0 is the descriptor's default, as it is everywhere (param.h).
//
// The editor never reads these: it draws the enum from the options the device
// sends with the parameter. They are here so that a patch *file* can say
// "1/8" and "triplet" rather than 7 and 3, and `app/test/app.test.mjs` checks
// them against the module's own option names.
export const METRONOME_DIVISIONS = ['8 bars', '4 bars', '2 bars', '1 bar',
                                    '1/2', '1/4', '1/8', '1/16', '1/32', '1/64'];
export const METRONOME_FEELS = ['straight', 'dotted', 'triplet'];

// MasterClock::Source. Not in a generated enum - master_clock.h is a class
// body, which the generator deliberately does not parse - so the values are
// spelled out and the comment says where they come from.
export const CLOCK_SOURCES = [
  { value: 0, label: 'internal', hint: 'the module’s own tempo' },
  { value: 1, label: 'CV / analogue sync', hint: 'pulses on the sync jack' },
  { value: 2, label: 'MIDI clock', hint: 'F8 from a host or a drum machine' },
];

// SysexHandler::SwapTiming: where a Program Change recall lands.
export const SWAP_TIMINGS = [
  { value: 0, label: 'immediately' },
  { value: 1, label: 'on the next beat' },
  { value: 2, label: 'on the next bar' },
];

export const CC_TARGET_KINDS = labelled(P.CcTargetKind, {
  CC_TARGET_NODE: 'a node parameter',
  CC_TARGET_CLOCK: 'the clock',
  CC_TARGET_TRANSPORT: 'the transport',
  CC_TARGET_PORT: 'a MIDI port (not built yet)',
  CC_TARGET_KEY: 'the key',
  CC_TARGET_KINDS: '',
}, 'CcTargetKind').filter((k) => k.label && k.key !== 'CC_TARGET_KINDS');

export const CLOCK_TARGETS = labelled(P.CcClockTarget, {
  CC_CLOCK_TEMPO: 'tempo (BPM)',
  CC_CLOCK_SOURCE: 'clock source',
  CC_CLOCK_PPQN: 'CV pulses per quarter',
  CC_CLOCK_TARGETS: '',
}, 'CcClockTarget').filter((t) => t.label);

// The key is one setting for the whole patch and no node carries a copy, so
// it is reached by kind rather than by node and parameter (src/node/patch.h).
export const KEY_TARGETS = labelled(P.CcKeyTarget, {
  CC_KEY_ROOT: 'root',
  CC_KEY_SCALE: 'scale',
  CC_KEY_OCTAVE: 'register',
  CC_KEY_TARGETS: '',
}, 'CcKeyTarget').filter((t) => t.label);

export const TRANSPORT_TARGETS = labelled(P.CcTransportTarget, {
  CC_TRANSPORT_START: 'start',
  CC_TRANSPORT_STOP: 'stop',
  CC_TRANSPORT_CONTINUE: 'continue',
  CC_TRANSPORT_TAP: 'tap tempo',
  CC_TRANSPORT_TARGETS: '',
}, 'CcTransportTarget').filter((t) => t.label);

// CcFlags is a bag of masks and values rather than a plain enum, so these are
// written out with the mask they live under and checked against the generated
// values, which is what keeps them honest.
const flag = (key) => {
  if (!(key in P.CcFlags)) throw new Error(`CcFlags has no ${key}`);
  return P.CcFlags[key];
};

export const TAKEOVER = {
  mask: flag('CC_TAKEOVER_MASK'),
  options: [
    { value: flag('CC_TAKEOVER_JUMP'), label: 'jump', hint: 'the parameter follows the knob at once' },
    { value: flag('CC_TAKEOVER_PICKUP'), label: 'pick-up', hint: 'nothing moves until the knob passes the current value' },
    { value: flag('CC_TAKEOVER_SCALE'), label: 'scale', hint: 'the knob nudges from where the value already is' },
  ],
};

export const RELATIVE = {
  mask: flag('CC_RELATIVE_MASK'),
  options: [
    { value: flag('CC_ABSOLUTE'), label: 'absolute', hint: 'an ordinary knob or fader' },
    { value: flag('CC_RELATIVE_TWOS'), label: 'relative (2’s complement)', hint: '1..63 up, 127..65 down' },
    { value: flag('CC_RELATIVE_SIGNED'), label: 'relative (signed bit)', hint: '1..63 up, 65..127 down' },
    { value: flag('CC_RELATIVE_OFFSET64'), label: 'relative (offset 64)', hint: '65..127 up, 63..1 down' },
  ],
};

export const FOURTEEN_BIT = flag('CC_FOURTEEN_BIT');
export const PASS_THROUGH = flag('CC_PASS_THROUGH');

// What kind of thing an algorithm is, and the order the picker shelves them
// in: what a patch is usually built out of first, down to the plumbing. The
// *values* come from the firmware, which is what stops a category being a
// guess made from an algorithm's name here - and `labelled` fails the moment
// the firmware gains or renames one, so a new shelf cannot arrive unnamed.
//
// CATEGORY_NONE is "other", and it is not a mistake: it is where an algorithm
// from firmware newer than this app lands, and where one from firmware older
// than the category itself lands. Either way the algorithm is still offered.
export const ALGORITHM_CATEGORIES = labelled(P.AlgorithmCategory, {
  CATEGORY_SEQUENCER: 'sequencers',
  CATEGORY_MIDI: 'notes and MIDI',
  CATEGORY_CLOCK: 'clock',
  CATEGORY_MODULATOR: 'modulators',
  CATEGORY_LOGIC: 'logic',
  CATEGORY_UTILITY: 'utility',
  CATEGORY_NONE: 'other',
}, 'AlgorithmCategory');

// Which way a jack faces, as the three states the firmware has. The label is
// what fits on a toggle; the hint is the sentence that says which way the
// signal actually runs, which is the part nobody should have to remember.
const GATE_DIRECTION_HINTS = {
  GATE_PORT_UNUSED: 'nothing is patched here',
  GATE_PORT_IN: 'the jack drives a gate bus',
  GATE_PORT_OUT: 'a gate bus drives the jack',
};

export const GATE_DIRECTIONS = labelled(P.GatePortDirection, {
  GATE_PORT_UNUSED: 'unused',
  GATE_PORT_IN: 'in',
  GATE_PORT_OUT: 'out',
}, 'GatePortDirection').map((d) => ({ ...d, hint: GATE_DIRECTION_HINTS[d.key] }));

// A MIDI channel selector's options, with 0 spelled out rather than left as a
// number a user has to know means omni.
export function channelLabel(channel) {
  return channel === 0 ? 'omni (any channel)' : `channel ${channel}`;
}
