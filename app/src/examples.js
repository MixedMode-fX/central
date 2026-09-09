// Example patches: something to open on a first visit, and something to take
// apart.
//
// The library starts empty, and an empty library in front of a machine with
// thirty algorithms is not a blank page - it is a wall. These are the patches
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

export const EXAMPLES = {
  'Default patch (main.cpp)': {
    about: 'What a freshly flashed module runs: the clock divided by 4 ticks pulses jack 1, and a sustain pedal on jack 8 sends CC 64 to every port. It is already running: watch jack 1 under play.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 8, dir: 'in', bus: 0 }],
      nodes: [{ algo: 'Sustain', in: [0], out: [0], params: [1, 64, 0] }, { algo: 'ClockDiv', out: [1], params: [0, 4] }],
      midi_out: [{ targets: ['ALL'], channel: 0, bus: 0 }],
    },
  },
  'Metronome: quarters on jack 1, bars on jack 2, and as notes': {
    about: 'Two dividers off the master clock: /24 is one pulse per beat, /96 one per bar. Both also become notes so you can hear them. Enable audio under play, then change the tempo while it runs.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 24] },
        { algo: 'ClockDiv', out: [1], params: [0, 96] },
        { algo: 'GateToNote', in: [0], out: [0], params: [72, 80, 1] },
        { algo: 'GateToNote', in: [1], out: [0], params: [60, 127, 1] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'MIDI thru with a sustain pedal on jack 8': {
    about: 'DIN 1 goes straight to USB 1. Enable audio under play, hold a key on the on-screen keyboard, hold jack 8 high and release the key: the note holds until the pedal comes up.',
    patch: {
      gate_ports: [{ port: 8, dir: 'in', bus: 0 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'Sustain', in: [0], out: [0], params: [1, 64, 0] }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'MIDI router: DIN 1 to USB 1 and DIN 2, USB 1 back to DIN 1': {
    about: 'Pure routing, no nodes. Play into DIN 1 under play and both USB 1 and DIN 2 receive; switch "into" to USB 1 and only DIN 1 receives; play into DIN 2 and nothing is accepted.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }, { sources: ['USB 1'], channel: 0, bus: 1 }],
      midi_out: [{ targets: ['USB 1', 'DIN 2'], channel: 0, bus: 0 }, { targets: ['DIN 1'], channel: 0, bus: 1 }],
    },
  },
  'Channel split and merge: DIN 1 ch 1 to USB 1, ch 2 to USB 2, host merged in': {
    about: 'Two input ports read the same DIN with different channel filters; a third merges the USB host into the first bus. Change the channel under play and watch which port the MIDI log says received it.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 1, bus: 0 }, { sources: ['DIN 1'], channel: 2, bus: 1 }, { sources: ['USB Host'], channel: 0, bus: 0 }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }, { targets: ['USB 2'], channel: 0, bus: 1 }],
    },
  },
  'Chord and transpose: one key becomes a major triad an octave up': {
    about: 'DIN 1 → Chord (root, +4, +7) → Transpose +12 → USB 1. Every note-on becomes three, and every note-off releases exactly those three.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'Chord', in: [0], out: [1], params: [2, 4, 7] }, { algo: 'Transpose', in: [1], out: [2], params: [12] }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Mono bass: lowest held note wins': {
    about: 'DIN 1 → NotePriority (lowest) → USB 1. Hold several keys: only the lowest sounds, and releasing it hands the voice to the next lowest.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'NotePriority', in: [0], out: [1], params: [0] }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Arpeggiator clocked by the module: hold a chord': {
    about: 'A /6 divider (sixteenths) advances the arpeggiator, up-down over two octaves with 60 ms gates. Enable audio under play and hold two or three keys. Change the tempo while it plays — or turn the hold parameter on and let go of the keys.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'Arpeggiator', in: [0, 0], out: [1], params: [2, 2, 60, 0] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'In key: a chord voiced from the module\u2019s scale, arpeggiated and held': {
    about: 'The module is in A minor, and nothing in the patch names a scale - so the chord voicer follows it. One key becomes a diatonic triad (0, 2 and 4 steps of the scale), the arpeggiator holds it, and a /6 divider plays it. Press one key and let go: it keeps running. Change the key under \u201cMIDI\u201d and the whole patch moves.',
    patch: {
      globals: { scale: 'minor', root: 9 },
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'Chord', in: [0], out: [1], params: [2, 2, 4] },
        { algo: 'Arpeggiator', in: [1, 0], out: [2], params: [0, 2, 60, 0, 1] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Euclidean drums: E(3,8), E(5,8), E(2,8) on jacks 1-3 and as notes': {
    about: 'One /6 divider advances three Euclidean sequencers in lock-step. Each fires a jack and a note (36, 42, 38). Enable audio under play; jacks 1 to 3 light in turn, and the three lanes are audible as a kit.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 2 }, { port: 3, dir: 'out', bus: 3 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'EuclidianSequencer', in: [0], out: [1], params: [8, 0, 0, 3, 0] },
        { algo: 'EuclidianSequencer', in: [0], out: [2], params: [8, 0, 0, 5, 2] },
        { algo: 'EuclidianSequencer', in: [0], out: [3], params: [8, 0, 0, 2, 4] },
        { algo: 'GateToNote', in: [1], out: [0], params: [36, 127, 10] },
        { algo: 'GateToNote', in: [2], out: [0], params: [42, 80, 10] },
        { algo: 'GateToNote', in: [3], out: [0], params: [38, 110, 10] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Step sequencer with probability: 16 steps, 70 % of them get through': {
    about: 'A 16-step pattern, written as hits, on a /6 divider, turned into notes and thinned by Probability. Jack 1 shows the full pattern, the notes what survived — and the step grid on the patch tab outlines the step being played.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'StepSequencer', in: [0], out: [1], seq: { hits: 'x.x.x..xx.x..x.x' } },
        { algo: 'GateToNote', in: [1], out: [0], params: [60, 100, 1] },
        { algo: 'Probability', in: [0], out: [1], params: [70, 0] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Note sequencer: a bass line in C minor, transposed from the keyboard': {
    about: 'Degrees, not notes: an 8-step line in C minor on a /6 divider, with a rest, an accent, a two-step note and a tie. Enable audio under play, then play a key: the root inlet re-pitches the running line and the note lane on the patch tab, without changing the stored degrees.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'NoteSequencer', in: [0, null, 0], out: [1], seq: { scale: 'minor', root: 48, channel: 1,
            steps: [0, { deg: 0, accent: true }, '-', 3, { deg: 5, len: 2 }, '-', { deg: 4, vel: 80 }, '='] } },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Poly sequencer: four chords, four voices, 60 % gate': {
    about: 'One step per beat, up to four degrees a step in C major, each voice with its own velocity. The 60 % gate is an estimate from the measured step period, so the first chord after a tempo change is the wrong length, on purpose. Change the scale to "dorian" in the JSON under library → files and load it back: same pattern, different colour.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 24] },
        { algo: 'PolySequencer', in: [0], out: [1], seq: { scale: 'major', root: 60, gate: 60,
            steps: [{ deg: [0, 2, 4, 7] }, { deg: [3, 5, 7], vel: [90, 70, 70] }, { deg: [4, 6, 8, 11], vel: [100, 80, 80, 60] }, { deg: [3, 5, 7, 9], len: 1, accent: true }] } },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Drum sequencer to jacks: kick, snare and a 12-step hat on jacks 1-3': {
    about: 'One DrumSeqGate, three lanes patched to jacks and five left unconnected. The hat lane is 12 steps against 16, so the pattern realigns every 48 steps. Lane 4 is the accent for the kick: a second gate lane, the modular way. Tap jack 5 under play to reset every lane.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'out', bus: 2 }, { port: 3, dir: 'out', bus: 3 }, { port: 4, dir: 'out', bus: 4 }, { port: 5, dir: 'in', bus: 5 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'DrumSeqGate', in: [0, 5], out: [1, 2, 3, 4, null, null, null, null], seq: { length: 16,
            lanes: ['x...x...x...x...', '....x.......x...', { hits: 'x.x.x.x.x.x.', length: 12 }, 'x.......x.......'] } },
      ],
    },
  },
  'Drum sequencer to MIDI: a General MIDI kit on channel 10': {
    about: 'DrumSeqMidi: a note number per lane, a velocity per cell (x = 100, X = 127, o = 60), 20 ms notes, every note-off from the ledger. The open hat lane fires 60 % of the time. Enable audio under play: channel 10 gets a percussive voice.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'DrumSeqMidi', in: [0], out: [1], seq: { length: 16, gate: 20, lanes: [
            { note: 36, hits: 'X...x..X..x.X...' },
            { note: 38, hits: '....X.......X..o' },
            { note: 42, hits: 'o.x.o.x.o.x.o.x.' },
            { note: 46, hits: '..o...o...o...x.', prob: 60 },
            { note: 39, hits: '..............x.', channel: 10 } ] } },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Random sequencer: pulse jack 2 to shred a new pattern': {
    about: 'A 16-step random pattern at 40 % density on a /6 divider, to jack 1 and to a note. Tap jack 2 under play to draw a new pattern; hold jack 3 to reset it to step 1.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'in', bus: 5 }, { port: 3, dir: 'in', bus: 6 }],
      nodes: [
        { algo: 'ClockDiv', out: [0], params: [0, 6] },
        { algo: 'RandomSequencer', in: [0, 6, 5], out: [1], params: [16, 0, 0, 40, 0] },
        { algo: 'GateToNote', in: [1], out: [0], params: [48, 100, 1] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Divider chain: /24 on jack 1, then /4 of that on jack 2': {
    about: 'The second ClockDiv has its inlet connected, so it divides rising edges of the first instead of the master count. Jack 2 pulses once per bar.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [{ algo: 'ClockDiv', out: [0], params: [0, 24] }, { algo: 'ClockDiv', in: [0], out: [1], params: [0, 4] }],
    },
  },
  'Logic: AND, OR, XOR of jacks 1 and 2 on jacks 3-5, NOT of jack 1 on jack 6': {
    about: 'Give jacks 1 and 2 different rates under play and read the truth table off jacks 3 to 6.',
    patch: {
      gate_ports: [{ port: 1, dir: 'in', bus: 0 }, { port: 2, dir: 'in', bus: 1 }, { port: 3, dir: 'out', bus: 2 }, { port: 4, dir: 'out', bus: 3 }, { port: 5, dir: 'out', bus: 4 }, { port: 6, dir: 'out', bus: 5 }],
      nodes: [{ algo: 'AND', in: [0, 1], out: [2] }, { algo: 'OR', in: [0, 1], out: [3] }, { algo: 'XOR', in: [0, 1], out: [4] }, { algo: 'NOT', in: [0], out: [5] }],
    },
  },
  'Feedback: a NOT on its own bus oscillates at half the pass rate': {
    about: 'Buses are double-buffered, so feedback is a one-pass delay rather than a hang. Jack 1 flickers rather than the patch hanging.',
    patch: { gate_ports: [{ port: 1, dir: 'out', bus: 0 }], nodes: [{ algo: 'NOT', in: [0], out: [0] }] },
  },
};
