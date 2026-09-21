// Example patches: something to open on a first visit, and something to take
// apart.
//
// The library starts empty, and an empty library in front of a machine with
// this many algorithms is not a blank page - it is a wall. These are the patches
// the emulator page used to open with: each exercises one part of the machine,
// says what to do and what to expect, and is small enough to read.
//
// They are written in the JSON dialect of `patchjson.js` - algorithms by name,
// jacks numbered from 1, sequencers as patterns rather than bytes - so they
// are also the worked examples of that format. Loading one is loading a file:
// it lands in the editor unsaved, and saving it makes it yours.
//
// `app/test/app.test.mjs` loads every one of them into the real firmware and
// fails if it is refused, so an example cannot rot into a patch that no longer
// validates.
//
// Each one carries the shelf it belongs on. A flat list of thirty patches is
// the same wall an empty library is, so the picker shelves them
// (`exampleGroups` below) exactly as the add bar shelves the algorithms.

import { PITCH_CLASSES } from './music.js';

// The shelves, in the order they are read. A category this list does not know
// gets a shelf of its own at the end rather than disappearing into another
// one - the same rule `core/catalogue.js` follows for an algorithm from newer
// firmware.
export const EXAMPLE_CATEGORIES = [
  'starting points',
  'performance',
  'harmony',
  'melody',
  'rhythm',
  'arrangement',
  'generative',
  'modulation',
  'clocks & logic',
  'routing & MIDI',
];

export const EXAMPLES = {
  'Pedal and pulse': {
    category: 'starting points',
    about: 'A metronome at a quarter note pulses jack 1, and a sustain pedal on jack 8 sends CC 64 to every port. A module boots empty, so this is the smallest patch worth opening: it is already running \u2014 watch jack 1 under play.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 8, dir: 'in', bus: 0 }],
      nodes: [{ algo: 'Sustain', in: [0], out: [0], params: [1, 64, 0] }, { algo: 'Metronome', out: [1], seq: { division: '1/4' } }],
      midi_out: [{ port: 1, targets: ['ALL'], channel: 0, bus: 0 }],
    },
  },
  'Two sources, one output': {
    category: 'routing & MIDI',
    about: 'A merge. Two arpeggios run on their own note buses \u2014 one from the keyboard, one a chord built on the key \u2014 and MIDI out 1 reads both of them at once (`bus: [0, 1]`). Neither source has to give up its own bus to be summed, which is why a port names a set of buses rather than one: jack 1 still pulses from bus 1 alone.',
    patch: {
      midi_in: [{ port: 1, sources: ['ALL'], channel: 0, bus: 2 }],
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/8' } },
        { algo: 'Arpeggiator', in: [2, 0], out: [0], params: [2, 2, 60, 0] },
        { algo: 'Chord', in: [2], out: [1] },
      ],
      midi_out: [{ port: 1, targets: ['ALL'], channel: 0, bus: [0, 1] }],
    },
  },
  'Metronome': {
    category: 'clocks & logic',
    about: 'Two metronomes off the master clock, set by note value rather than by divisor: 1/4 is the beat, and 1/8 triplet is three in the space of two. Both also become notes so you can hear them play against each other. Enable audio under play, then change the tempo \u2014 or either division \u2014 while it runs.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/4' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/8', feel: 'triplet' } },
        { algo: 'GateToNote', in: [0], out: [0], params: [72, 80, 1] },
        { algo: 'GateToNote', in: [1], out: [0], params: [60, 127, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'MIDI thru with a pedal': {
    category: 'routing & MIDI',
    about: 'DIN 1 goes straight to USB 1. Enable audio under play, hold a key on the on-screen keyboard, hold jack 8 high and release the key: the note holds until the pedal comes up.',
    patch: {
      gate_ports: [{ port: 8, dir: 'in', bus: 0 }],
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'Sustain', in: [0], out: [0], params: [1, 64, 0] }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'MIDI router': {
    category: 'routing & MIDI',
    about: 'Pure routing, no nodes. Play into DIN 1 under play and both USB 1 and DIN 2 receive; switch "into" to USB 1 and only DIN 1 receives; play into DIN 2 and nothing is accepted.',
    patch: {
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }, { port: 2, sources: ['USB 1'], channel: 0, bus: 1 }],
      midi_out: [{ port: 1, targets: ['USB 1', 'DIN 2'], channel: 0, bus: 0 }, { port: 2, targets: ['DIN 1'], channel: 0, bus: 1 }],
    },
  },
  'Channel split and merge': {
    category: 'routing & MIDI',
    about: 'Two input ports read the same DIN with different channel filters; a third merges the USB host into the first bus. Change the channel under play and watch which port the MIDI log says received it.',
    patch: {
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 1, bus: 0 }, { port: 2, sources: ['DIN 1'], channel: 2, bus: 1 }, { port: 3, sources: ['USB host'], channel: 0, bus: 0 }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }, { port: 2, targets: ['USB 2'], channel: 0, bus: 1 }],
    },
  },
  'Chord and transpose': {
    category: 'harmony',
    about: 'DIN 1 → Chord → Transpose +12 → USB 1. Nothing names a key, so the triad is the plain major one: root, +4, +7. Every note-on becomes three, and every note-off releases exactly those three.',
    patch: {
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'Chord', in: [0], out: [1] }, { algo: 'Transpose', in: [1], out: [2], params: [140] }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Mono bass': {
    category: 'routing & MIDI',
    about: 'DIN 1 → NotePriority (lowest) → USB 1. Hold several keys: only the lowest sounds, and releasing it hands the voice to the next lowest.',
    patch: {
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'NotePriority', in: [0], out: [1], params: [0] }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Arpeggiator': {
    category: 'harmony',
    about: 'A metronome at a sixteenth advances the arpeggiator, up-down over two octaves with 60 ms gates. Enable audio under play and hold two or three keys. Change the tempo while it plays — or turn the hold parameter on and let go of the keys.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Arpeggiator', in: [0, 0], out: [1], params: [2, 2, 60, 0] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'In key': {
    category: 'harmony',
    about: 'The module is in A minor, and nothing in the patch names a scale - so the chord voicer follows it. One key becomes a diatonic triad — the “triad” quality is steps of the scale, so it is minor here — the arpeggiator holds it, and a sixteenth-note metronome plays it. Press one key and let go: it keeps running. Change the key under \u201ckey\u201d and the whole patch moves.',
    patch: {
      globals: { scale: 'minor', root: 9 },
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Chord', in: [0], out: [1] },
        { algo: 'Arpeggiator', in: [1, 0], out: [2], params: [0, 2, 60, 0, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Self-playing chord': {
    category: 'harmony',
    about: 'Nothing is plugged in \u2014 no keyboard, no gate, no MIDI. The chord voicer has nothing patched to its note inlet, so it plays itself: the triad of the key, held indefinitely. A sixteenth-note metronome arpeggiates it up over two octaves, and a four-step note sequencer clocked at one bar walks the root through C minor (i \u2013 VI \u2013 iv \u2013 v), re-voicing the chord each time. The GateHold on jack 2 is the run switch: with nothing patched into it either, its "gate" parameter is the level it sends, and the AND lets the sixteenths through only while it is up. Enable audio under play, then turn that one parameter off and on.',
    patch: {
      globals: { scale: 'minor', root: 0 },
      gate_ports: [{ port: 1, dir: 'out', bus: 2 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Metronome', out: [3], seq: { division: '1 bar' } },
        { algo: 'GateHold', out: [1], params: [1, 0, 1] },
        { algo: 'AND', in: [0, 1], out: [2] },
        { algo: 'NoteSequencer', in: [3], out: [0], seq: { length: 4, octave: 4, steps: [0, 5, 3, 4] } },
        { algo: 'Chord', in: [0], out: [1], params: [1, 0, 0, 4] },
        { algo: 'Arpeggiator', in: [1, 2], out: [2], params: [0, 2, 60, 0] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Chord stabs': {
    category: 'harmony',
    about: 'Nothing is plugged in. Chord has nothing on its note inlet, so it plays the tonic of the key and holds it \u2014 a pad, and on its own that is a patch of one chord that never moves. Retrigger is what makes it a part: a Euclidean 7-in-16 off the sixteenth-note metronome drives its trigger, and every edge releases the chord and sends it again at a 1/32, which is a stab. The rhythm is the Euclid\u2019s and the harmony is the Chord\u2019s; neither knows about the other. Jack 1 shows the pattern. Enable audio under play, then move the Euclidean pulses \u2014 or set Retrigger\u2019s release to \u201ctie\u201d and hear the same rhythm re-articulate a chord that never stops.',
    patch: {
      globals: { scale: 'minor', root: 2 },
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'EuclidianSequencer', in: [0], out: [1], params: [16, 0, 0, 7, 2] },
        { algo: 'Chord', in: [null], out: [0], params: [1, 0, 0, 4] },
        { algo: 'Retrigger', in: [0, 1], out: [1], params: [9, 1, 1, 0] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Euclidean drums': {
    category: 'rhythm',
    about: 'One sixteenth-note metronome advances three Euclidean sequencers in lock-step. Each fires a jack and a note (36, 42, 38). Enable audio under play; jacks 1 to 3 light in turn, and the three note numbers are read as General MIDI \u2014 kick, closed hat, snare \u2014 by the kit under listen \u2192 drums.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 2 }, { port: 3, dir: 'out', bus: 3 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'EuclidianSequencer', in: [0], out: [1], params: [8, 0, 0, 3, 0] },
        { algo: 'EuclidianSequencer', in: [0], out: [2], params: [8, 0, 0, 5, 2] },
        { algo: 'EuclidianSequencer', in: [0], out: [3], params: [8, 0, 0, 2, 4] },
        { algo: 'GateToNote', in: [1], out: [0], params: [36, 127, 10] },
        { algo: 'GateToNote', in: [2], out: [0], params: [42, 80, 10] },
        { algo: 'GateToNote', in: [3], out: [0], params: [38, 110, 10] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Step sequencer': {
    category: 'rhythm',
    about: 'A 16-step pattern, written as hits, at a sixteenth, turned into notes and thinned by Probability. Jack 1 shows the full pattern, the notes what survived — and the step grid on the patch tab outlines the step being played.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'StepSequencer', in: [0], out: [1], seq: { hits: 'x.x.x..xx.x..x.x' } },
        { algo: 'GateToNote', in: [1], out: [0], params: [60, 100, 1] },
        { algo: 'Probability', in: [0], out: [1], params: [70, 0] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Trig conditions': {
    category: 'rhythm',
    about: 'One eighth-note pulse, thinned by a condition rather than by dice. The first GateProbability is set to 1:2, so jack 1 takes every other eighth \u2014 the kick. Its "decision" outlet carries that answer as a gate, so a NOT and an AND give the second one exactly the eighths the first refused \u2014 the hat, interlocked by construction. That patch is why there is no "neighbour" setting to find: the decision is a cable, and it reaches anything. Enable audio under play, then set the first node\u2019s condition to 1:4 or 3:4 and watch both patterns move together.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 3 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/8' } },
        { algo: 'GateProbability', in: [0], out: [1, null, 2], params: [0, 4, 0] },
        { algo: 'NOT', in: [2], out: [4] },
        { algo: 'AND', in: [0, 4], out: [3] },
        { algo: 'GateToNote', in: [1], out: [0], params: [36, 120, 10] },
        { algo: 'GateToNote', in: [3], out: [0], params: [42, 90, 10] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Call and response': {
    category: 'rhythm',
    about: 'The same eighth-note pulse into one GateProbability at even odds, played twice: the gates it passes are the kick on jack 1, and the gates it refused leave by its "dropped" outlet and play the hat on jack 2 \u2014 two interlocking patterns off one node, no NOT and no AND. Probability has the same outlet for notes, which no cable can build. Enable audio under play and move "chance": the hits cross from one voice to the other instead of leaving holes.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 2 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/8' } },
        { algo: 'GateProbability', in: [0], out: [1, 2], params: [50, 1, 0] },
        { algo: 'GateToNote', in: [1], out: [0], params: [36, 120, 10] },
        { algo: 'GateToNote', in: [2], out: [0], params: [42, 90, 10] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Note sequencer': {
    category: 'melody',
    about: 'Degrees, not notes: an 8-step line in C minor at a sixteenth, with a rest, an accent, a two-step note and a tie. Enable audio under play, then play a key: the root inlet re-pitches the running line and the note lane on the patch tab, without changing the stored degrees.',
    patch: {
      globals: { scale: 'minor', root: 0 },
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'NoteSequencer', in: [0, null, 0], out: [1], seq: { octave: 4, channel: 1,
            steps: [0, { deg: 0, accent: true }, '-', 3, { deg: 5, len: 2 }, '-', { deg: 4, vel: 80 }, '='] } },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Poly sequencer': {
    category: 'melody',
    about: 'One step per beat, up to four degrees a step in C major, each voice with its own velocity. The 60 % gate is an estimate from the measured step period, so the first chord after a tempo change is the wrong length, on purpose. Change the scale to "dorian" in the JSON under library → files and load it back: same pattern, different colour.',
    patch: {
      globals: { scale: 'major', root: 0 },
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/4' } },
        { algo: 'PolySequencer', in: [0], out: [1], seq: { octave: 5, gate: 60,
            steps: [{ deg: [0, 2, 4, 7] }, { deg: [3, 5, 7], vel: [90, 70, 70] }, { deg: [4, 6, 8, 11], vel: [100, 80, 80, 60] }, { deg: [3, 5, 7, 9], len: 1, accent: true }] } },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Drum sequencer to jacks': {
    category: 'rhythm',
    about: 'One DrumSeqGate, three lanes patched to jacks and five left unconnected. The hat lane is 12 steps against 16, so the pattern realigns every 48 steps. Lane 4 is the accent for the kick: a second gate lane, the modular way. Tap jack 5 under play to reset every lane. Enable audio and it plays a kit: a gate lane has no note number, so the drum is the one the firmware sends for that lane.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 2 }, { port: 3, dir: 'out', bus: 3 }, { port: 4, dir: 'out', bus: 4 }, { port: 5, dir: 'in', bus: 5 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'DrumSeqGate', in: [0, 5], out: [1, 2, 3, 4, null, null, null, null], seq: { length: 16,
            lanes: ['x...x...x...x...', '....x.......x...', { hits: 'x.x.x.x.x.x.', length: 12 }, 'x.......x.......'] } },
      ],
    },
  },
  'Drum sequencer to MIDI': {
    category: 'rhythm',
    about: 'DrumSeqMidi: a note number per lane, a velocity per cell (x = 100, X = 127, o = 60), 20 ms notes, every note-off from the ledger. The open hat lane fires 60 % of the time. Enable audio under play: it gets its own kit and level under listen \u2192 drums, and each lane plays the drum its note number means.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'DrumSeqMidi', in: [0], out: [1], seq: { length: 16, gate: 20, lanes: [
            { note: 36, hits: 'X...x..X..x.X...' },
            { note: 38, hits: '....X.......X..o' },
            { note: 42, hits: 'o.x.o.x.o.x.o.x.' },
            { note: 46, hits: '..o...o...o...x.', prob: 60 },
            { note: 39, hits: '..............x.', channel: 10 } ] } },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Random sequencer': {
    category: 'rhythm',
    about: 'A 16-step random pattern at 40 % density at a sixteenth, to jack 1 and to a note. Tap jack 2 under play to draw a new pattern; hold jack 3 to reset it to step 1.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'in', bus: 5 }, { port: 3, dir: 'in', bus: 6 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'RandomSequencer', in: [0, 6, 5], out: [1], params: [16, 0, 0, 40, 0] },
        { algo: 'GateToNote', in: [1], out: [0], params: [48, 100, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Locked to the transport': {
    category: 'clocks & logic',
    about: 'Two gate sequencers at different lengths \u2014 five steps against eight \u2014 off one sixteenth-note metronome, so they walk in and out of phase with each other. Transport is the third node: MIDI start, stop and continue reach the clock and never a bus, and this is what puts them back on one. Its start outlet is wired to both sequencers\u2019 reset inlets, so pressing start under play drops both patterns back to step one together, wherever the phrase had got to \u2014 jack 3 flashes the trigger itself. A sequencer counts edges rather than the clock\u2019s count, so without this a stopped and restarted DAW would have it resume mid-pattern. Enable audio under play, then press stop and start a few bars apart.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 3 }, { port: 2, dir: 'out', bus: 4 }, { port: 3, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Transport', out: [1] },
        { algo: 'StepSequencer', in: [0, 1], out: [3], seq: { hits: 'x..x.' } },
        { algo: 'EuclidianSequencer', in: [0, 1], out: [4], params: [8, 0, 0, 3, 0] },
        { algo: 'GateToNote', in: [3], out: [0], params: [36, 127, 10] },
        { algo: 'GateToNote', in: [4], out: [0], params: [42, 80, 10] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Divider chain': {
    category: 'clocks & logic',
    about: 'The ClockDiv has its inlet connected, so it divides rising edges of the metronome instead of the master count \u2014 which is how a rate this module does not name by note value still gets built. Jack 1 is the beat, jack 2 pulses once per bar.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [{ algo: 'Metronome', out: [0], seq: { division: '1/4' } }, { algo: 'ClockDiv', in: [0], out: [1], params: [0, 4] }],
    },
  },
  'Logic': {
    category: 'clocks & logic',
    about: 'Give jacks 1 and 2 different rates under play and read the truth table off jacks 3 to 6.',
    patch: {
      gate_ports: [{ port: 1, dir: 'in', bus: 0 }, { port: 2, dir: 'in', bus: 1 }, { port: 3, dir: 'out', bus: 2 }, { port: 4, dir: 'out', bus: 3 }, { port: 5, dir: 'out', bus: 4 }, { port: 6, dir: 'out', bus: 5 }],
      nodes: [{ algo: 'AND', in: [0, 1], out: [2] }, { algo: 'OR', in: [0, 1], out: [3] }, { algo: 'XOR', in: [0, 1], out: [4] }, { algo: 'NOT', in: [0], out: [5] }],
    },
  },
  'Gate hold': {
    category: 'clocks & logic',
    about: 'Every clock source in this module makes a 5 ms trigger, which is right for clocking and useless for holding anything open. GateHold is the missing piece. Jack 1 latches high on the first beat and stays there until you tap jack 8 to reset it; jack 2 gets the same trigger stretched into a 200 ms gate.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 2 }, { port: 8, dir: 'in', bus: 3 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1 bar' } },
        { algo: 'GateHold', in: [0, 3], out: [1], params: [1, 0] },
        { algo: 'GateHold', in: [0], out: [2], params: [3, 200] },
      ],
    },
  },
  'Modulation': {
    category: 'modulation',
    about: 'The LFO writes a control bus rather than a note bus, and a modulation route points that signal at the sequencer\u2019s probability \u2014 as if a knob somewhere were being turned for you, but at twelve bits rather than a CC\u2019s seven. Enable audio under play and listen to the pattern thin out and fill in over eight bars. On the canvas the modulated parameter is the extra socket on the sequencer; drag the LFO\u2019s outlet onto any block to point it somewhere else.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'RandomSequencer', in: [0], out: [1], params: [16, 0, 0, 50, 0] },
        { algo: 'LFO', out: [0], params: [2, 2, 0, 1, 1, 255, 0, 0, 2] },
        { algo: 'GateToNote', in: [1], out: [0], params: [48, 100, 1] },
      ],
      mod_map: [{ slot: 0, bus: 0, targetKind: 0, targetIndex: 1, param: 3,
                  min: 0, max: 0, depth: 255, flags: 0 }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Sample and hold': {
    category: 'modulation',
    about: 'The oldest modular utility there is. SampleHold takes one reading of its own noise on every trigger and holds it steady between triggers; a modulation route turns that held level into the transposition a sequence is played at, so the melody moves in whole steps rather than sliding. Slew is patched between them \u2014 set its rise and fall above zero under play to hear the steps become glides.',
    patch: {
      globals: { scale: 'minor', root: 0 },
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/4' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/16' } },
        { algo: 'SampleHold', in: [0], out: [0], params: [3, 1, 5] },
        { algo: 'Slew', in: [0], out: [1] },
        { algo: 'NoteSequencer', in: [1], out: [0], seq: { length: 8, octave: 5, steps: [0, 2, 4, 2, 5, 4, 2, 0] } },
        { algo: 'Transpose', in: [0], out: [1] },
      ],
      mod_map: [{ slot: 0, bus: 1, targetKind: 0, targetIndex: 5, param: 0,
                  min: 128, max: 140, depth: 255, flags: 0 }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Stepped modulation': {
    category: 'modulation',
    about: 'The same idea as sample and hold, with the randomness taken out of it. StepMod draws a shape \u2014 a triangle here \u2014 and cuts it into eight steps; every trigger on jack 1\u2019s clock moves it one step along, so one period is eight beats and the speed is whatever is clocking it rather than a rate set to match. A modulation route turns that stepped level into the transposition the melody is played at, so the tune walks an octave up and back down in even steps and lands where it started. Under play, set direction to pendulum or random, or steps to 3 against the sequencer\u2019s 8, and the same eight notes come out as a different phrase.',
    patch: {
      globals: { scale: 'minor', root: 0 },
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/8' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/16' } },
        { algo: 'StepMod', in: [0], out: [0], params: [2, 8, 0, 255, 0, 2] },
        { algo: 'NoteSequencer', in: [1], out: [0], seq: { length: 8, octave: 4, steps: [0, 2, 4, 2, 5, 4, 2, 0] } },
        { algo: 'Transpose', in: [0], out: [1] },
      ],
      mod_map: [{ slot: 0, bus: 0, targetKind: 0, targetIndex: 4, param: 0,
                  min: 128, max: 140, depth: 255, flags: 0 }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'MIDI to CV and gate': {
    category: 'routing & MIDI',
    about: 'The converter that makes everything upstream reach something that is not a MIDI instrument. Play the keyboard under play: jack 1 is the gate, held for as long as a key is, and jack 2 is the trigger it fires on every attack. The pitch outlet is a control signal \u2014 twelve bits, one of them a fraction of a semitone so the wheel is not stepped \u2014 and a modulation route reads it straight back into a note here, which is what the DAC will do in volts. Five octaves of range from C2, so the note that comes back is the note you played; bend the wheel and it bends with you.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      midi_in: [{ port: 1, sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'MidiToCV', in: [0], out: [0, 0, 1, 2, 1], params: [3, 5, 36, 2, 1, 0, 1, 1] },
        { algo: 'GateToNote', in: [0], out: [1], params: [36, 100, 1] },
      ],
      mod_map: [{ slot: 0, bus: 0, targetKind: 0, targetIndex: 1, param: 0,
                  min: 36, max: 96, depth: 255, flags: 0 }],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Generative': {
    category: 'generative',
    about: 'Nothing is plugged in, and nothing in it was typed. The module is in A minor. Harmony walks the degrees of the key a bar at a time and Chord voices a triad on each root, so the quality of every chord falls out of the key rather than out of a setting. Its walk is weighed rather than looked up \u2014 at fifths 80 the roots mostly fall by fifths and spread 30 hardens that into something close to a loop, which is what the circle of fifths under its controls is drawing. It writes the first four chords down and repeats them (loop 4), and drift 6 redraws one of them now and then and keeps it, so the progression is recognisably itself and never quite the same twice. Turing is an eight-bit loop with chaos at 8: it repeats, and about one step in twelve changes each time round — its pulse drives the automaton and its level becomes the melody, so the bar where the rhythm moves is the bar where the tune moves. NoteDelay echoes that melody a dotted eighth later, two scale steps up, with spread pushing each echo further off the grid. The automaton is Wolfram rule 110 on a ring of eight; the three lanes on jacks 1 to 3 are neighbours, so a figure walks across them. Once every two bars a slow LFO crosses a comparator and sends the register back to the pattern it grew out of. Enable audio under play, then move Turing’s chaos, Harmony’s fifths, or the delay’s spread while it runs.',
    patch: {
      globals: { scale: 'minor', root: 9 },
      gate_ports: [{ port: 1, dir: 'out', bus: 3 }, { port: 2, dir: 'out', bus: 4 }, { port: 3, dir: 'out', bus: 5 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1 bar' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/8' } },
        { algo: 'Harmony', in: [0], out: [0, 0], params: [4, 75, 0, 4, 3, 80, 1, 0, 80, 25, 40, 30, 6] },
        { algo: 'Chord', in: [0], out: [1] },
        { algo: 'Turing', in: [1, 6], out: [2, 1], params: [8, 8, 5, 1, 21, 0, 1] },
        { algo: 'CvToNote', in: [1, 1, null], out: [2], params: [1, 4, 2, 1, 2, 0, 90, 2] },
        { algo: 'NoteDelay', in: [2, null], out: [3], params: [1, 7, 2, 25, 3, 2, 65, 100, 8, 0, 1] },
        { algo: 'Automaton', in: [2, null], out: [3, 4, 5, null, null, null, null, null], params: [110, 129, 1, 2, 8, 100, 0] },
        { algo: 'LFO', out: [2], params: [1, 2, 0, 3, 1, 255, 0, 0, 2] },
        { algo: 'CvToGate', in: [2], out: [6], params: [90, 10, 2, 2, 0, 0] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 1 }, { port: 2, targets: ['USB 1'], channel: 0, bus: 3 }],
    },
  },
  // --- the played ones ---------------------------------------------------
  //
  // Four finished pieces rather than four demonstrations. Each runs on its
  // own with nothing plugged in, each is in a key worth hearing, and each
  // hands the surface over: the eight pots are macros the patch names, so
  // loading one re-silkscreens the panel (ui/surface/Surface.js).
  //
  // The two things a performer reaches for are wired the same way in all
  // four, because a controller that meant something different in every patch
  // is a controller nobody learns:
  //
  //   * **the pads move the key.** Pads send notes 36..51 on the surface's
  //     factory cable, so a NoteFilter over that range into a Key node makes
  //     the bottom sixteen pads sixteen roots. Nothing in the patch stores a
  //     pitch, so the whole thing moves at once.
  //   * **the keyboard plays over it.** Everything from 52 up is the other
  //     half of the split, and lands wherever that patch has somewhere for a
  //     person to play.
  //
  // A macro destination is a window, and past the top of its window it holds
  // (src/node/patch.h). So a pot with three destinations on three windows is
  // an arrangement: the parts arrive one after another as it comes up, and
  // stay.
  'Aurora': {
    category: 'performance',
    about: 'E lydian at 84, and the pots are the piece. Harmony walks the key a bar at a time, Chord voices a seventh on each root and Voicer moves as few notes as it can between them, so the pad changes without ever jumping; a Euclidean 5-in-16 re-strikes it through Retrigger. The melody is an eight-step Turing register read as degrees, echoed a dotted eighth later, two scale steps down each time. Enable audio under play, then go to the surface: pot 1 “bloom” is the arrangement — Euclidean pulses first, then the delay’s repeats, then the seventh becomes a ninth at the top. Pot 3 “colour” steps the key itself from lydian to mixolydian to dorian, pot 4 is the tempo, pot 5 opens the canon out and mutes the dry line at the top of its travel, and pot 6 lifts the register. Play the keyboard and your notes are snapped into the key and taken by the same delay; hit a pad and everything moves to that root.',
    patch: {
      globals: { scale: 'lydian', root: 4, bpm: 84 },
      gate_ports: [{ port: 1, dir: 'out', bus: 2 }, { port: 2, dir: 'out', bus: 3 }, { port: 3, dir: 'out', bus: 0 }],
      midi_in: [{ port: 1, sources: ['USB 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1 bar' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/16' } },
        { algo: 'Metronome', out: [4], seq: { division: '1/8' } },
        { algo: 'Harmony', in: [0], out: [4, 0], params: [6, 70, 0, 8, 0, 0, 0, 0, 55, 0, 0, 45, 12] },
        { algo: 'Chord', in: [4], out: [5], params: [2, 1, 0, 0, 70] },
        { algo: 'Voicer', in: [5], out: [6], params: [1, 48, 79, 0, 1, 0] },
        { algo: 'EuclidianSequencer', in: [1], out: [2], params: [16, 0, 0, 5, 0] },
        { algo: 'Retrigger', in: [6, 2], out: [7], params: [5, 1, 1, 0] },
        { algo: 'Turing', in: [4, null], out: [3, 1], params: [8, 12, 8, 1, 0, 0, 1] },
        { algo: 'CvToNote', in: [1, 3, null], out: [3], params: [1, 5, 2, 3, 2, 0, 80, 1] },
        { algo: 'NoteFilter', in: [0], out: [1], params: [0, 52, 127, 1, 127, 1] },
        { algo: 'Note Quantise', in: [1, null], out: [3] },
        { algo: 'NoteFilter', in: [0], out: [2], params: [0, 36, 51, 1, 127, 1] },
        { algo: 'Key', in: [2], params: [0, 1] },
        { algo: 'NoteDelay', in: [3, null], out: [7], params: [1, 7, 2, 0, 4, 254, 70, 100, 12, 0, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 7 }],
      macros: [
        { index: 0, name: 'bloom' }, { index: 1, name: 'drift' }, { index: 2, name: 'colour' },
        { index: 3, name: 'tempo' }, { index: 4, name: 'echo' }, { index: 5, name: 'lift' },
      ],
      macro_dest: [
        { slot: 0, macro: 0, targetIndex: 6, param: 3, srcHi: 200, depth: 7 },
        { slot: 1, macro: 0, targetIndex: 14, param: 4, srcLo: 110, depth: 3 },
        { slot: 2, macro: 0, targetIndex: 4, param: 0, srcLo: 190, srcHi: 190, depth: 1 },
        { slot: 3, macro: 1, targetIndex: 3, param: 12, depth: 60 },
        { slot: 4, macro: 1, targetIndex: 3, param: 8, depth: -30 },
        { slot: 5, macro: 1, targetIndex: 8, param: 1, depth: 40 },
        { slot: 6, macro: 2, targetKind: 4, param: 1, srcLo: 96, srcHi: 96, depth: 1 },
        { slot: 7, macro: 2, targetKind: 4, param: 1, srcLo: 176, srcHi: 176, depth: -3 },
        { slot: 8, macro: 3, targetKind: 1, param: 0, depth: 36 },
        { slot: 9, macro: 4, targetIndex: 14, param: 8, depth: 90 },
        { slot: 10, macro: 4, targetIndex: 14, param: 6, depth: 25 },
        { slot: 11, macro: 4, targetIndex: 14, param: 10, srcLo: 210, srcHi: 210, depth: 1 },
        { slot: 12, macro: 5, targetKind: 4, param: 2, srcLo: 128, srcHi: 128, depth: 1 },
        { slot: 13, macro: 5, targetIndex: 5, param: 2, depth: 12 },
      ],
      cc_map: [
        { slot: 0, sources: ['USB 1'], cc: 20, targetKind: 5, targetIndex: 0 },
        { slot: 1, sources: ['USB 1'], cc: 21, targetKind: 5, targetIndex: 1 },
        { slot: 2, sources: ['USB 1'], cc: 22, targetKind: 5, targetIndex: 2 },
        { slot: 3, sources: ['USB 1'], cc: 23, targetKind: 5, targetIndex: 3 },
        { slot: 4, sources: ['USB 1'], cc: 24, targetKind: 5, targetIndex: 4 },
        { slot: 5, sources: ['USB 1'], cc: 25, targetKind: 5, targetIndex: 5 },
      ],
    },
  },
  'Ember': {
    category: 'performance',
    about: 'D phrygian at 132: drums, a bass line and a stabbing lead, and the two bottom pads are its mute buttons. Pad 1 and pad 2 each drive a MidiToCV trigger into a GateHold in toggle mode, and the AND under it is what the part is clocked through — so a pad press drops the drums or the bass out and the next one brings them back, on the beat, with no parameter moved. Enable audio under play and try it. Pot 2 “stutter” cuts the drum pattern from sixteen steps to twelve and then to seven, which re-bars the whole kit against a lead that is still counting sixteen; pot 6 “scale” walks the key from phrygian to blues to pentatonic minor. The keyboard is the bass line’s root inlet, so a key you hold re-pitches the sequence without changing a degree of it, and pads 5 to 16 move the key everything else is in.',
    patch: {
      globals: { scale: 'phrygian', root: 2, bpm: 132 },
      gate_ports: [{ port: 1, dir: 'out', bus: 6 }, { port: 2, dir: 'out', bus: 7 }, { port: 3, dir: 'out', bus: 9 }],
      midi_in: [{ port: 1, sources: ['USB 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'NoteFilter', in: [0], out: [1], params: [0, 36, 36, 1, 127, 1] },
        { algo: 'NoteFilter', in: [0], out: [2], params: [0, 37, 37, 1, 127, 1] },
        { algo: 'NoteFilter', in: [0], out: [3], params: [0, 40, 51, 1, 127, 1] },
        { algo: 'NoteFilter', in: [0], out: [4], params: [0, 52, 127, 1, 127, 1] },
        { algo: 'MidiToCV', in: [1], out: [null, null, null, null, 2] },
        { algo: 'MidiToCV', in: [2], out: [null, null, null, null, 3] },
        { algo: 'GateHold', in: [2, null], out: [4], params: [2, 0, 1] },
        { algo: 'GateHold', in: [3, null], out: [5], params: [2, 0, 1] },
        { algo: 'AND', in: [0, 4], out: [6] },
        { algo: 'AND', in: [0, 5], out: [7] },
        { algo: 'Key', in: [3], params: [0, 1] },
        { algo: 'DrumSeqMidi', in: [6], out: [5], seq: { length: 16, gate: 25, lanes: [
            { note: 36, hits: 'X...x...X..x..x.' },
            { note: 38, hits: '....X.......X..o' },
            { note: 42, hits: 'o.x.o.x.o.x.o.x.', prob: 90 },
            { note: 46, hits: '..o...o...o...X.', prob: 55 } ] } },
        { algo: 'NoteSequencer', in: [7, null, 4], out: [6], seq: { length: 16, octave: 3, gate: 35,
            steps: [0, '-', 0, 0, '-', 3, '-', 0, 0, '-', 1, '-', 0, 6, '-', { deg: 5, accent: true }] } },
        { algo: 'EuclidianSequencer', in: [0, null], out: [9], params: [16, 0, 0, 7, 2] },
        { algo: 'Turing', in: [0, null], out: [8, 0], params: [16, 14, 8, 1, 0, 0, 1] },
        { algo: 'CvToNote', in: [0, 9, null], out: [7], params: [1, 4, 2, 3, 2, 0, 95, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 5 }, { port: 2, targets: ['USB 1'], channel: 0, bus: 6 },
                 { port: 3, targets: ['USB 1'], channel: 0, bus: 7 }],
      macros: [
        { index: 0, name: 'drive' }, { index: 1, name: 'stutter' }, { index: 2, name: 'lead' },
        { index: 3, name: 'tempo' }, { index: 4, name: 'mutate' }, { index: 5, name: 'scale' },
      ],
      macro_dest: [
        { slot: 0, macro: 0, targetIndex: 13, param: 4, depth: 80 },
        { slot: 1, macro: 0, targetIndex: 13, param: 7, depth: 40 },
        { slot: 2, macro: 0, targetIndex: 12, param: 2, srcLo: 140, depth: 40 },
        { slot: 3, macro: 1, targetIndex: 12, param: 0, srcLo: 128, srcHi: 128, depth: -4 },
        { slot: 4, macro: 1, targetIndex: 12, param: 0, srcLo: 200, srcHi: 200, depth: -5 },
        { slot: 5, macro: 2, targetIndex: 14, param: 3, depth: 5 },
        { slot: 6, macro: 2, targetIndex: 16, param: 2, depth: 4 },
        { slot: 7, macro: 2, targetIndex: 16, param: 5, srcLo: 160, depth: 90 },
        { slot: 8, macro: 3, targetKind: 1, param: 0, depth: 44 },
        { slot: 9, macro: 4, targetIndex: 15, param: 1, depth: 45 },
        { slot: 10, macro: 4, targetIndex: 13, param: 1, srcLo: 170, srcHi: 170, depth: 2 },
        { slot: 11, macro: 5, targetKind: 4, param: 1, srcLo: 100, srcHi: 100, depth: -2 },
        { slot: 12, macro: 5, targetKind: 4, param: 1, srcLo: 190, srcHi: 190, depth: -1 },
      ],
      cc_map: [
        { slot: 0, sources: ['USB 1'], cc: 20, targetKind: 5, targetIndex: 0 },
        { slot: 1, sources: ['USB 1'], cc: 21, targetKind: 5, targetIndex: 1 },
        { slot: 2, sources: ['USB 1'], cc: 22, targetKind: 5, targetIndex: 2 },
        { slot: 3, sources: ['USB 1'], cc: 23, targetKind: 5, targetIndex: 3 },
        { slot: 4, sources: ['USB 1'], cc: 24, targetKind: 5, targetIndex: 4 },
        { slot: 5, sources: ['USB 1'], cc: 25, targetKind: 5, targetIndex: 5 },
      ],
    },
  },
  'Glasswork': {
    category: 'performance',
    about: 'G whole tone at 76 — a scale with no leading note and no tonic to fall to, so a progression in it hangs rather than resolves. Harmony walks it anyway and its roots land on a bus that your keyboard also writes; NotePriority takes the latest of the two, so playing a key interrupts the machine for as long as you hold it and hands the piece back when you let go. Chord builds a wide sus2 on whatever won, Mirror reflects it about the key’s axis, and an up-down arpeggio holds it while a 1/16 delay blurs the edges. Pot 1 “shadow” sweeps Mirror’s amount from nothing to the whole negative-harmony reflection and then steps it to plain inversion at the top — one knob, the same notes, an entirely different piece. An LFO on a modulation route is already breathing the delay’s chance over two bars, so it never plays the same bar twice.',
    patch: {
      globals: { scale: 'whole tone', root: 7, bpm: 76 },
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      midi_in: [{ port: 1, sources: ['USB 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1 bar' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/16' } },
        { algo: 'NoteFilter', in: [0], out: [3], params: [0, 52, 127, 1, 127, 1] },
        { algo: 'NoteFilter', in: [0], out: [2], params: [0, 36, 51, 1, 127, 1] },
        { algo: 'Key', in: [2], params: [0, 1] },
        { algo: 'Harmony', in: [0], out: [3, 0], params: [4, 80, 0, 4, 0, 0, 0, 0, 70, 0, 0, 30, 5] },
        { algo: 'NotePriority', in: [3], out: [4], params: [2] },
        { algo: 'Chord', in: [4], out: [5], params: [5, 4, 0, 3, 90] },
        { algo: 'Mirror', in: [5, null], out: [1], params: [1, 1, 0, 0] },
        { algo: 'Arpeggiator', in: [1, 1, null, null], out: [6], params: [2, 2, 90, 0, 1, 1] },
        { algo: 'NoteDelay', in: [6, null], out: [7], params: [1, 8, 1, 0, 3, 0, 60, 80, 40, 0, 1] },
        { algo: 'LFO', out: [1], params: [1, 2, 0, 3, 1, 255, 0, 0, 2] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 7 }],
      mod_map: [{ slot: 0, bus: 1, targetKind: 0, targetIndex: 10, param: 7,
                  min: 40, max: 100, depth: 255, flags: 0 }],
      macros: [
        { index: 0, name: 'shadow' }, { index: 1, name: 'phrase' }, { index: 2, name: 'flutter' },
        { index: 3, name: 'tempo' }, { index: 4, name: 'scale' }, { index: 5, name: 'rain' },
      ],
      macro_dest: [
        { slot: 0, macro: 0, targetIndex: 8, param: 1, depth: 99 },
        { slot: 1, macro: 0, targetIndex: 8, param: 0, srcLo: 220, srcHi: 220, depth: 1 },
        { slot: 2, macro: 1, targetIndex: 5, param: 0, depth: 10 },
        { slot: 3, macro: 1, targetIndex: 5, param: 3, srcLo: 140, depth: 8 },
        { slot: 4, macro: 1, targetIndex: 5, param: 1, depth: -50 },
        { slot: 5, macro: 2, targetIndex: 9, param: 1, depth: 1 },
        { slot: 6, macro: 2, targetIndex: 9, param: 2, depth: -60 },
        { slot: 7, macro: 3, targetKind: 1, param: 0, depth: 44 },
        { slot: 8, macro: 4, targetKind: 4, param: 1, srcLo: 110, srcHi: 110, depth: -3 },
        { slot: 9, macro: 4, targetKind: 4, param: 1, srcLo: 200, srcHi: 200, depth: -5 },
        { slot: 10, macro: 5, targetIndex: 10, param: 4, depth: 5 },
        { slot: 11, macro: 5, targetIndex: 10, param: 8, depth: 80 },
        { slot: 12, macro: 5, targetIndex: 10, param: 6, depth: 35 },
      ],
      cc_map: [
        { slot: 0, sources: ['USB 1'], cc: 20, targetKind: 5, targetIndex: 0 },
        { slot: 1, sources: ['USB 1'], cc: 21, targetKind: 5, targetIndex: 1 },
        { slot: 2, sources: ['USB 1'], cc: 22, targetKind: 5, targetIndex: 2 },
        { slot: 3, sources: ['USB 1'], cc: 23, targetKind: 5, targetIndex: 3 },
        { slot: 4, sources: ['USB 1'], cc: 24, targetKind: 5, targetIndex: 4 },
        { slot: 5, sources: ['USB 1'], cc: 25, targetKind: 5, targetIndex: 5 },
      ],
    },
  },
  'Foundry': {
    category: 'performance',
    about: 'A harmonic minor at 152. The drums are not a pattern: a Wolfram automaton on a ring of eight runs at a sixteenth and four of its cells are the kit, so the figure walks across the lanes and never quite repeats — jacks 1 to 4 show it. The harmony is a Tonnetz cycle, chromatic triads a bar apart where one voice moves a semitone at a time, arpeggiated at random over two octaves; the bass is a twelve-step Turing register read as degrees. Pot 1 “rule” walks the automaton’s rule number down from 110 through 90 to 30 — the drum part being rewritten under your hand, from a figure that drifts to Sierpiński triangles to noise with structure in it — and takes three lanes away at the top. Pot 4 “scale” walks the key from harmonic minor to melodic minor to plain minor, which is the difference between the piece sounding lit and sounding flat. Play the keyboard and your notes are snapped into the key and land on the lead; hit a pad and the Tonnetz starts its next cycle from that root.',
    patch: {
      globals: { scale: 'harmonic minor', root: 9, bpm: 152 },
      gate_ports: [{ port: 1, dir: 'out', bus: 2 }, { port: 2, dir: 'out', bus: 3 },
                   { port: 3, dir: 'out', bus: 4 }, { port: 4, dir: 'out', bus: 5 }],
      midi_in: [{ port: 1, sources: ['USB 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Metronome', out: [1], seq: { division: '1 bar' } },
        { algo: 'NoteFilter', in: [0], out: [1], params: [0, 52, 127, 1, 127, 1] },
        { algo: 'NoteFilter', in: [0], out: [2], params: [0, 36, 51, 1, 127, 1] },
        { algo: 'Automaton', in: [0, null], out: [2, 3, 4, 5], params: [110, 137, 1, 2, 8, 100, 0] },
        { algo: 'GateToNote', in: [2], out: [4], params: [36, 120, 10] },
        { algo: 'GateToNote', in: [3], out: [4], params: [42, 80, 10] },
        { algo: 'GateToNote', in: [4], out: [4], params: [38, 110, 10] },
        { algo: 'GateToNote', in: [5], out: [4], params: [46, 65, 10] },
        { algo: 'Tonnetz', in: [1, null, 2], out: [3], params: [1, 20, 1, 4, 95, 1, 0] },
        { algo: 'Arpeggiator', in: [3, 0, null, null], out: [5], params: [3, 2, 45, 0, 1, 1] },
        { algo: 'Note Quantise', in: [1, null], out: [5] },
        { algo: 'Turing', in: [0, null], out: [6, 0], params: [12, 18, 8, 1, 0, 0, 1] },
        { algo: 'CvToNote', in: [0, 6, null], out: [6], params: [1, 2, 2, 3, 2, 0, 100, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 4 }, { port: 2, targets: ['USB 1'], channel: 0, bus: 5 },
                 { port: 3, targets: ['USB 1'], channel: 0, bus: 6 }],
      macros: [
        { index: 0, name: 'rule' }, { index: 1, name: 'swarm' }, { index: 2, name: 'tempo' },
        { index: 3, name: 'scale' }, { index: 4, name: 'tilt' }, { index: 5, name: 'bass' },
      ],
      macro_dest: [
        { slot: 0, macro: 0, targetIndex: 4, param: 0, depth: -80 },
        { slot: 1, macro: 0, targetIndex: 4, param: 4, srcLo: 200, depth: -3 },
        { slot: 2, macro: 1, targetIndex: 10, param: 1, depth: 2 },
        { slot: 3, macro: 1, targetIndex: 10, param: 2, depth: 80 },
        { slot: 4, macro: 2, targetKind: 1, param: 0, srcHi: 200, depth: 28 },
        { slot: 5, macro: 3, targetKind: 4, param: 1, srcLo: 120, srcHi: 120, depth: 1 },
        { slot: 6, macro: 3, targetKind: 4, param: 1, srcLo: 200, srcHi: 200, depth: -2 },
        { slot: 7, macro: 4, targetIndex: 9, param: 1, depth: 70 },
        { slot: 8, macro: 4, targetIndex: 9, param: 2, srcLo: 210, srcHi: 210, depth: -1 },
        { slot: 9, macro: 5, targetIndex: 12, param: 1, depth: 55 },
        { slot: 10, macro: 5, targetIndex: 13, param: 2, depth: 4 },
      ],
      cc_map: [
        { slot: 0, sources: ['USB 1'], cc: 20, targetKind: 5, targetIndex: 0 },
        { slot: 1, sources: ['USB 1'], cc: 21, targetKind: 5, targetIndex: 1 },
        { slot: 2, sources: ['USB 1'], cc: 22, targetKind: 5, targetIndex: 2 },
        { slot: 3, sources: ['USB 1'], cc: 23, targetKind: 5, targetIndex: 3 },
        { slot: 4, sources: ['USB 1'], cc: 24, targetKind: 5, targetIndex: 4 },
        { slot: 5, sources: ['USB 1'], cc: 25, targetKind: 5, targetIndex: 5 },
      ],
    },
  },
  // --- arrangements ------------------------------------------------------
  //
  // A patch with parts. Every part below runs all the time on its own bus;
  // a switch decides which one is heard, and what moves the switch is the
  // form of the piece - a counter's carry, a pattern of bars, a hand.
  'Verse and chorus': {
    category: 'arrangement',
    about: 'A song of two parts, four bars each. The verse and the chorus are two note sequencers and two drum grids, all four running all the time on their own buses; a NoteSwitch per voice decides which pair is heard. What moves the switches is a Counter clocked once a bar at length four: its carry fires on the edge that wraps the count — every fourth bar — and steps both switches together, and jacks 1 to 3 show the bar count in binary while it waits. Transport start resets the counter, the switches and every sequencer at once, so pressing play under play always begins on bar one of the verse. Enable audio, let it run eight bars, then set the Counter’s length to 2 or 8 under play and the form changes without a note being moved.',
    patch: {
      globals: { scale: 'minor', root: 9, bpm: 118 },
      gate_ports: [{ port: 1, dir: 'out', bus: 3 }, { port: 2, dir: 'out', bus: 4 }, { port: 3, dir: 'out', bus: 5 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Metronome', out: [1], seq: { division: '1 bar' } },
        { algo: 'Transport', out: [2] },
        { algo: 'Counter', in: [1, 2], out: [3, null, 4, 5], params: [4] },
        { algo: 'NoteSequencer', in: [0, 2], out: [0], seq: { length: 16, octave: 3, gate: 45,
            steps: [0, '-', '-', 0, '-', '-', 3, '-', 0, '-', '-', 0, '-', 5, '-', 4] } },
        { algo: 'NoteSequencer', in: [0, 2], out: [1], seq: { length: 16, octave: 4, gate: 55,
            steps: [{ deg: 0, accent: true }, 2, 4, 2, 5, 4, 2, 0, { deg: 3, accent: true }, 5, 7, 5, 4, 2, 0, '-'] } },
        { algo: 'NoteSwitch', in: [0, 1, null, null, null, null, 3, 2], out: [2] },
        { algo: 'DrumSeqMidi', in: [0, 2], out: [3], seq: { length: 16, gate: 20, lanes: [
            { note: 36, hits: 'x.......x.......' },
            { note: 42, hits: 'o.o.o.o.o.o.o.o.' } ] } },
        { algo: 'DrumSeqMidi', in: [0, 2], out: [4], seq: { length: 16, gate: 20, lanes: [
            { note: 36, hits: 'X...x..X..x.X...' },
            { note: 38, hits: '....X.......X..o' },
            { note: 42, hits: 'o.x.o.x.o.x.o.x.' },
            { note: 46, hits: '..o...o...o...X.' } ] } },
        { algo: 'NoteSwitch', in: [3, 4, null, null, null, null, 3, 2], out: [5] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 2 }, { port: 2, targets: ['USB 1'], channel: 0, bus: 5 }],
    },
  },
  'Fill bars': {
    category: 'arrangement',
    about: 'Which bars are the fill is a pattern. A StepSequencer clocked once a bar plays an eight-bar pattern with a hit on bars four and eight; a D FlipFlop clocked by the same bar takes that trigger and holds it for exactly the bar, and GateToCV turns the held bit into a level. That level is the `select` inlet of two NoteSwitches — the bass and the drums — so on a hit bar both play their fill part and on every other bar their main part, and jack 1 is high for the whole of every fill bar. Nothing here is a mode: the pattern is the form, and rewriting it under play rewrites the song. Enable audio and edit the StepSequencer’s hits.',
    patch: {
      globals: { scale: 'minor', root: 2, bpm: 124 },
      gate_ports: [{ port: 1, dir: 'out', bus: 4 }, { port: 2, dir: 'out', bus: 3 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Metronome', out: [1], seq: { division: '1 bar' } },
        { algo: 'Transport', out: [2] },
        { algo: 'StepSequencer', in: [1, 2], out: [3], seq: { hits: '...x...x' } },
        { algo: 'FlipFlop', in: [3, null, 1, null], out: [4], params: [1] },
        { algo: 'GateToCV', in: [4], out: [0] },
        { algo: 'NoteSequencer', in: [0, 2], out: [0], seq: { length: 16, octave: 3, gate: 50,
            steps: [0, '-', 0, '-', 0, '-', 3, '-', 0, '-', 0, '-', 6, '-', 5, '-'] } },
        { algo: 'NoteSequencer', in: [0, 2], out: [1], seq: { length: 16, octave: 3, gate: 60,
            steps: [0, 1, 2, 3, 4, 5, 6, 7, { deg: 7, accent: true }, 6, 5, 4, 3, 2, 1, { deg: 0, accent: true }] } },
        { algo: 'NoteSwitch', in: [0, 1, null, null, null, 0], out: [2] },
        { algo: 'DrumSeqMidi', in: [0, 2], out: [3], seq: { length: 16, gate: 20, lanes: [
            { note: 36, hits: 'X...x...X...x...' },
            { note: 38, hits: '....X.......X...' },
            { note: 42, hits: 'o.x.o.x.o.x.o.x.' } ] } },
        { algo: 'DrumSeqMidi', in: [0, 2], out: [4], seq: { length: 16, gate: 20, lanes: [
            { note: 36, hits: 'X.......X.......' },
            { note: 38, hits: 'oooo1111xxxxXXXX' },
            { note: 45, hits: '............X.X.' },
            { note: 49, hits: '...............X' } ] } },
        { algo: 'NoteSwitch', in: [3, 4, null, null, null, 0], out: [5] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 2 }, { port: 2, targets: ['USB 1'], channel: 0, bus: 5 }],
    },
  },
  'Passing the melody': {
    category: 'arrangement',
    about: 'One tune, three voices, a bar each. An eight-step line in E pentatonic goes into a NoteRouter that steps once a bar over three outlets: the first plays it as it is, the second through Transpose an octave up, the third through Chord as triads, so the same phrase moves from voice to voice round the room. A router releases what it has sounding on the outlet it leaves before the next one hears anything, which is why no voice is ever left holding a note. The GateRouter under it is the same switch with a held gate on its inlet — a decoder — so jacks 1 to 3 light with the voice that is playing. Both step from the same bar and both reset from transport start, so they cannot disagree. Enable audio under play, then set the NoteRouter’s steps to 2 and the third voice drops out of the rota.',
    patch: {
      globals: { scale: 'pentatonic minor', root: 4, bpm: 96 },
      gate_ports: [{ port: 1, dir: 'out', bus: 4 }, { port: 2, dir: 'out', bus: 5 }, { port: 3, dir: 'out', bus: 6 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/8' } },
        { algo: 'Metronome', out: [1], seq: { division: '1 bar' } },
        { algo: 'Transport', out: [2] },
        { algo: 'NoteSequencer', in: [0, 2], out: [0], seq: { length: 8, octave: 4, gate: 60,
            steps: [0, 2, 4, 5, 4, 2, { deg: 1, accent: true }, 3] } },
        { algo: 'NoteRouter', in: [0, null, 1, 2], out: [4, 2, 3] },
        { algo: 'Transpose', in: [2], out: [4], params: [140] },
        { algo: 'Chord', in: [3], out: [4], params: [1, 0, 0, 5] },
        { algo: 'GateHold', out: [3], params: [1, 0, 1] },
        { algo: 'GateRouter', in: [3, null, 1, 2], out: [4, 5, 6] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 4 }],
    },
  },
  'Shift register canon': {
    category: 'rhythm',
    about: 'A rhythm chasing itself. A Euclidean 5-in-16 is the data of a ShiftRegister clocked at the same sixteenth, so tap 3 plays the figure an eighth late, tap 5 a quarter late and tap 7 a dotted quarter late — a delay line measured in steps, not milliseconds, so it stays in time whatever the tempo does. Each tap plays its own note, an A minor arpeggio rising as the echoes fade, and jacks 1 to 4 show the figure walking down the register. Enable audio under play and move the Euclid’s pulses or rotation: every echo moves with it, in step. Then set the register’s loop on and the Euclid’s pulses to zero — what was in the register when you did goes round for ever.',
    patch: {
      globals: { scale: 'minor', root: 9, bpm: 100 },
      gate_ports: [{ port: 1, dir: 'out', bus: 2 }, { port: 2, dir: 'out', bus: 3 }, { port: 3, dir: 'out', bus: 4 }, { port: 4, dir: 'out', bus: 5 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'EuclidianSequencer', in: [0], out: [1], params: [16, 0, 0, 5, 0] },
        { algo: 'ShiftRegister', in: [0, 1], out: [2, null, 3, null, 4, null, 5] },
        { algo: 'GateToNote', in: [2], out: [0], params: [57, 115, 1] },
        { algo: 'GateToNote', in: [3], out: [0], params: [60, 95, 1] },
        { algo: 'GateToNote', in: [4], out: [0], params: [64, 75, 1] },
        { algo: 'GateToNote', in: [5], out: [0], params: [69, 55, 1] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Binary kit': {
    category: 'clocks & logic',
    about: 'A drum kit with no pattern in it. A Counter clocked at an eighth counts to eight, and its bits are the kit: /2 is a square at every other eighth, which is the hat on the off-beats; /4 rises on beats two and four, which is the snare; /8 rises on beat three and falls on beat one, and an Edge turns both of its edges into the kick. Jacks 1 to 3 show the three bits. In front of the counter, a D FlipFlop clocked once a bar is the mute button: its data is jack 8, so holding the jack anywhere in a bar stops the kit on the next downbeat and letting go brings it back on the downbeat after, never mid-bar — the flip-flop is what makes a button land on the beat. Enable audio under play, then set the counter’s length to 6 or 12 and the same three bits are a different groove.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 7 }, { port: 2, dir: 'out', bus: 8 }, { port: 3, dir: 'out', bus: 9 }, { port: 8, dir: 'in', bus: 3 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/8' } },
        { algo: 'Metronome', out: [1], seq: { division: '1 bar' } },
        { algo: 'Transport', out: [2] },
        { algo: 'FlipFlop', in: [3, null, 1, null], out: [null, 4], params: [1] },
        { algo: 'AND', in: [0, 4], out: [5] },
        { algo: 'Counter', in: [5, 2], out: [6, null, 7, 8, 9], params: [8] },
        { algo: 'GateToNote', in: [7], out: [0], params: [42, 70, 10] },
        { algo: 'GateToNote', in: [8], out: [0], params: [38, 110, 10] },
        { algo: 'Edge', in: [9], out: [10, 10] },
        { algo: 'GateToNote', in: [10], out: [0], params: [36, 125, 10] },
      ],
      midi_out: [{ port: 1, targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Feedback': {
    category: 'clocks & logic',
    about: 'Buses are double-buffered, so feedback is a one-pass delay rather than a hang. Jack 1 flickers rather than the patch hanging.',
    patch: { gate_ports: [{ port: 1, dir: 'out', bus: 0 }], nodes: [{ algo: 'NOT', in: [0], out: [0] }] },
  },
};

// --- what the picker shows ---------------------------------------------------

// The row's second line: what the patch says about itself, which is its first
// sentence. The rest of `about` is read once it has been loaded, on the page
// it is describing.
const firstSentence = (about) => {
  const at = about.indexOf('. ');
  return at < 0 ? about : about.slice(0, at + 1);
};

// And the line beside the name, read off the patch rather than written down
// beside it: the tempo and the key when it names them, and how big it is when
// it does not.
function shape(patch) {
  const globals = patch.globals ?? {};
  const parts = [];
  if (globals.bpm) parts.push(`${globals.bpm} BPM`);
  if (globals.scale && globals.scale !== 'chromatic') {
    parts.push(`${PITCH_CLASSES[(globals.root ?? 0) % 12]} ${globals.scale}`);
  }
  const n = (patch.nodes ?? []).length;
  if (parts.length < 2) parts.push(`${n} node${n === 1 ? '' : 's'}`);
  return parts.join(' \u00b7 ');
}

// The examples as `components/Picker.js` wants them, which is the same shape
// `core/catalogue.js` returns for the algorithms. Pure: no DOM, so the shelves
// are checked in `app/test/app.test.mjs` with no browser in the room.
export function exampleGroups() {
  const shelves = new Map(EXAMPLE_CATEGORIES.map((c) => [c, []]));
  for (const [name, example] of Object.entries(EXAMPLES)) {
    if (!shelves.has(example.category)) shelves.set(example.category, []);
    shelves.get(example.category).push({
      value: name, label: name, note: shape(example.patch), hint: firstSentence(example.about),
    });
  }
  return [...shelves]
    .filter(([, options]) => options.length)
    .map(([label, options]) => ({ key: label, label, options }));
}
