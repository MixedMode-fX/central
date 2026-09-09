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
export const SCALES = labelled(P.ScaleId, {
  SCALE_CHROMATIC: 'chromatic',
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
  SCALE_COUNT: '',
}, 'ScaleId').filter((s) => s.label);

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

export const GATE_DIRECTIONS = labelled(P.GatePortDirection, {
  GATE_PORT_UNUSED: 'unused',
  GATE_PORT_IN: 'in (the jack drives a bus)',
  GATE_PORT_OUT: 'out (a bus drives the jack)',
}, 'GatePortDirection');

// A MIDI channel selector's options, with 0 spelled out rather than left as a
// number a user has to know means omni.
export function channelLabel(channel) {
  return channel === 0 ? 'omni (any channel)' : `channel ${channel}`;
}
