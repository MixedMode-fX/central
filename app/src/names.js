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
// "global" is not a scale but a reference to the module's own (its mask is
// zero, which is what the firmware reads as "follow the key"), and it is what
// an algorithm carries until someone names a scale on it.
export const SCALES = labelled(P.ScaleId, {
  SCALE_GLOBAL: 'global',
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

// The scales a *key* can be in: every one except the reference to itself.
export const KEY_SCALES = SCALES.filter((s) => s.value !== P.ScaleId.SCALE_GLOBAL);

// Pitch classes, for a root. Sharps rather than flats, because the firmware
// stores a pitch class and has no opinion about spelling.
export const PITCH_CLASSES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

// The id, for the places that store a ScaleId rather than a mask: an
// algorithm's `scale` parameter, and the key in the globals.
export function scaleIdOf(name) {
  if (typeof name === 'number') return name & 0xff;
  const found = SCALES.find((s) => s.label === String(name).toLowerCase().replace(/[_-]+/g, ' ').trim());
  if (!found) {
    throw new Error(`unknown scale "${name}" (one of ${SCALES.map((s) => s.label).join(', ')})`);
  }
  return found.value;
}

export const scaleName = (id) => SCALES.find((s) => s.value === id)?.label ?? String(id);

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
  CC_TARGET_KINDS: '',
}, 'CcTargetKind').filter((k) => k.label && k.key !== 'CC_TARGET_KINDS');

export const CLOCK_TARGETS = labelled(P.CcClockTarget, {
  CC_CLOCK_TEMPO: 'tempo (BPM)',
  CC_CLOCK_SOURCE: 'clock source',
  CC_CLOCK_PPQN: 'CV pulses per quarter',
  CC_CLOCK_TARGETS: '',
}, 'CcClockTarget').filter((t) => t.label);

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
