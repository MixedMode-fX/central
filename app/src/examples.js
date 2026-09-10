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
  'Default patch': {
    about: 'What a freshly flashed module runs: a metronome at a quarter note pulses jack 1, and a sustain pedal on jack 8 sends CC 64 to every port. It is already running: watch jack 1 under play.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 8, dir: 'in', bus: 0 }],
      nodes: [{ algo: 'Sustain', in: [0], out: [0], params: [1, 64, 0] }, { algo: 'Metronome', out: [1], seq: { division: '1/4' } }],
      midi_out: [{ targets: ['ALL'], channel: 0, bus: 0 }],
    },
  },
  'Metronome': {
    about: 'Two metronomes off the master clock, set by note value rather than by divisor: 1/4 is the beat, and 1/8 triplet is three in the space of two. Both also become notes so you can hear them play against each other. Enable audio under play, then change the tempo \u2014 or either division \u2014 while it runs.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/4' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/8', feel: 'triplet' } },
        { algo: 'GateToNote', in: [0], out: [0], params: [72, 80, 1] },
        { algo: 'GateToNote', in: [1], out: [0], params: [60, 127, 1] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'MIDI thru with a pedal': {
    about: 'DIN 1 goes straight to USB 1. Enable audio under play, hold a key on the on-screen keyboard, hold jack 8 high and release the key: the note holds until the pedal comes up.',
    patch: {
      gate_ports: [{ port: 8, dir: 'in', bus: 0 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'Sustain', in: [0], out: [0], params: [1, 64, 0] }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'MIDI router': {
    about: 'Pure routing, no nodes. Play into DIN 1 under play and both USB 1 and DIN 2 receive; switch "into" to USB 1 and only DIN 1 receives; play into DIN 2 and nothing is accepted.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }, { sources: ['USB 1'], channel: 0, bus: 1 }],
      midi_out: [{ targets: ['USB 1', 'DIN 2'], channel: 0, bus: 0 }, { targets: ['DIN 1'], channel: 0, bus: 1 }],
    },
  },
  'Channel split and merge': {
    about: 'Two input ports read the same DIN with different channel filters; a third merges the USB host into the first bus. Change the channel under play and watch which port the MIDI log says received it.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 1, bus: 0 }, { sources: ['DIN 1'], channel: 2, bus: 1 }, { sources: ['USB Host'], channel: 0, bus: 0 }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }, { targets: ['USB 2'], channel: 0, bus: 1 }],
    },
  },
  'Chord and transpose': {
    about: 'DIN 1 → Chord (root, +4, +7) → Transpose +12 → USB 1. Every note-on becomes three, and every note-off releases exactly those three.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'Chord', in: [0], out: [1], params: [2, 4, 7] }, { algo: 'Transpose', in: [1], out: [2], params: [12] }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Mono bass': {
    about: 'DIN 1 → NotePriority (lowest) → USB 1. Hold several keys: only the lowest sounds, and releasing it hands the voice to the next lowest.',
    patch: {
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [{ algo: 'NotePriority', in: [0], out: [1], params: [0] }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Arpeggiator': {
    about: 'A metronome at a sixteenth advances the arpeggiator, up-down over two octaves with 60 ms gates. Enable audio under play and hold two or three keys. Change the tempo while it plays — or turn the hold parameter on and let go of the keys.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Arpeggiator', in: [0, 0], out: [1], params: [2, 2, 60, 0] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'In key': {
    about: 'The module is in A minor, and nothing in the patch names a scale - so the chord voicer follows it. One key becomes a diatonic triad (0, 2 and 4 steps of the scale), the arpeggiator holds it, and a sixteenth-note metronome plays it. Press one key and let go: it keeps running. Change the key under \u201cMIDI\u201d and the whole patch moves.',
    patch: {
      globals: { scale: 'minor', root: 9 },
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Chord', in: [0], out: [1], params: [2, 2, 4] },
        { algo: 'Arpeggiator', in: [1, 0], out: [2], params: [0, 2, 60, 0, 1] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Self-playing chord': {
    about: 'Nothing is plugged in \u2014 no keyboard, no gate, no MIDI. The chord voicer has nothing patched to its note inlet, so it plays itself: the triad of the key, held indefinitely. A sixteenth-note metronome arpeggiates it up over two octaves, and a four-step note sequencer clocked at one bar walks the root through C minor (i \u2013 VI \u2013 iv \u2013 v), re-voicing the chord each time. The GateHold on jack 2 is the run switch: with nothing patched into it either, its "gate" parameter is the level it sends, and the AND lets the sixteenths through only while it is up. Enable audio under play, then turn that one parameter off and on.',
    patch: {
      globals: { scale: 'minor', root: 0 },
      gate_ports: [{ port: 1, dir: 'out', bus: 2 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'Metronome', out: [3], seq: { division: '1 bar' } },
        { algo: 'GateHold', out: [1], params: [1, 0, 1] },
        { algo: 'AND', in: [0, 1], out: [2] },
        { algo: 'NoteSequencer', in: [3], out: [0], seq: { length: 4, root: 48, steps: [0, 5, 3, 4] } },
        { algo: 'Chord', in: [null, 0], out: [1], params: [2, 2, 4, 0, 0, 0, 0, 0, 0, 4] },
        { algo: 'Arpeggiator', in: [1, 2], out: [2], params: [0, 2, 60, 0] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 2 }],
    },
  },
  'Euclidean drums': {
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
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Step sequencer': {
    about: 'A 16-step pattern, written as hits, at a sixteenth, turned into notes and thinned by Probability. Jack 1 shows the full pattern, the notes what survived — and the step grid on the patch tab outlines the step being played.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'StepSequencer', in: [0], out: [1], seq: { hits: 'x.x.x..xx.x..x.x' } },
        { algo: 'GateToNote', in: [1], out: [0], params: [60, 100, 1] },
        { algo: 'Probability', in: [0], out: [1], params: [70, 0] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Note sequencer': {
    about: 'Degrees, not notes: an 8-step line in C minor at a sixteenth, with a rest, an accent, a two-step note and a tie. Enable audio under play, then play a key: the root inlet re-pitches the running line and the note lane on the patch tab, without changing the stored degrees.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'NoteSequencer', in: [0, null, 0], out: [1], seq: { scale: 'minor', root: 48, channel: 1,
            steps: [0, { deg: 0, accent: true }, '-', 3, { deg: 5, len: 2 }, '-', { deg: 4, vel: 80 }, '='] } },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Poly sequencer': {
    about: 'One step per beat, up to four degrees a step in C major, each voice with its own velocity. The 60 % gate is an estimate from the measured step period, so the first chord after a tempo change is the wrong length, on purpose. Change the scale to "dorian" in the JSON under library → files and load it back: same pattern, different colour.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/4' } },
        { algo: 'PolySequencer', in: [0], out: [1], seq: { scale: 'major', root: 60, gate: 60,
            steps: [{ deg: [0, 2, 4, 7] }, { deg: [3, 5, 7], vel: [90, 70, 70] }, { deg: [4, 6, 8, 11], vel: [100, 80, 80, 60] }, { deg: [3, 5, 7, 9], len: 1, accent: true }] } },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Drum sequencer to jacks': {
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
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Random sequencer': {
    about: 'A 16-step random pattern at 40 % density at a sixteenth, to jack 1 and to a note. Tap jack 2 under play to draw a new pattern; hold jack 3 to reset it to step 1.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }, { port: 2, dir: 'in', bus: 5 }, { port: 3, dir: 'in', bus: 6 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/16' } },
        { algo: 'RandomSequencer', in: [0, 6, 5], out: [1], params: [16, 0, 0, 40, 0] },
        { algo: 'GateToNote', in: [1], out: [0], params: [48, 100, 1] },
      ],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Divider chain': {
    about: 'The ClockDiv has its inlet connected, so it divides rising edges of the metronome instead of the master count \u2014 which is how a rate this module does not name by note value still gets built. Jack 1 is the beat, jack 2 pulses once per bar.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      nodes: [{ algo: 'Metronome', out: [0], seq: { division: '1/4' } }, { algo: 'ClockDiv', in: [0], out: [1], params: [0, 4] }],
    },
  },
  'Logic': {
    about: 'Give jacks 1 and 2 different rates under play and read the truth table off jacks 3 to 6.',
    patch: {
      gate_ports: [{ port: 1, dir: 'in', bus: 0 }, { port: 2, dir: 'in', bus: 1 }, { port: 3, dir: 'out', bus: 2 }, { port: 4, dir: 'out', bus: 3 }, { port: 5, dir: 'out', bus: 4 }, { port: 6, dir: 'out', bus: 5 }],
      nodes: [{ algo: 'AND', in: [0, 1], out: [2] }, { algo: 'OR', in: [0, 1], out: [3] }, { algo: 'XOR', in: [0, 1], out: [4] }, { algo: 'NOT', in: [0], out: [5] }],
    },
  },
  'Gate hold': {
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
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 0 }],
    },
  },
  'Sample and hold': {
    about: 'The oldest modular utility there is. SampleHold takes one reading of its own noise on every trigger and holds it steady between triggers; a modulation route turns that held level into the transposition a sequence is played at, so the melody moves in whole steps rather than sliding. Slew is patched between them \u2014 set its rise and fall above zero under play to hear the steps become glides.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 1 }],
      nodes: [
        { algo: 'Metronome', out: [0], seq: { division: '1/4' } },
        { algo: 'Metronome', out: [1], seq: { division: '1/16' } },
        { algo: 'SampleHold', in: [0], out: [0], params: [3, 1, 5] },
        { algo: 'Slew', in: [0], out: [1] },
        { algo: 'NoteSequencer', in: [1], out: [0], seq: { length: 8, root: 60, scale: 'minor', steps: [0, 2, 4, 2, 5, 4, 2, 0] } },
        { algo: 'Transpose', in: [0], out: [1] },
      ],
      mod_map: [{ slot: 0, bus: 1, targetKind: 0, targetIndex: 5, param: 0,
                  min: 0, max: 12, depth: 255, flags: 0 }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'MIDI to CV and gate': {
    about: 'The converter that makes everything upstream reach something that is not a MIDI instrument. Play the keyboard under play: jack 1 is the gate, held for as long as a key is, and jack 2 is the trigger it fires on every attack. The pitch outlet is a control signal \u2014 twelve bits, one of them a fraction of a semitone so the wheel is not stepped \u2014 and a modulation route reads it straight back into a note here, which is what the DAC will do in volts. Five octaves of range from C2, so the note that comes back is the note you played; bend the wheel and it bends with you.',
    patch: {
      gate_ports: [{ port: 1, dir: 'out', bus: 0 }, { port: 2, dir: 'out', bus: 1 }],
      midi_in: [{ sources: ['DIN 1'], channel: 0, bus: 0 }],
      nodes: [
        { algo: 'MidiToCV', in: [0], out: [0, 0, 1, 2, 1], params: [3, 5, 36, 2, 1, 0, 1, 1] },
        { algo: 'GateToNote', in: [0], out: [1], params: [36, 100, 1] },
      ],
      mod_map: [{ slot: 0, bus: 0, targetKind: 0, targetIndex: 1, param: 0,
                  min: 36, max: 96, depth: 255, flags: 0 }],
      midi_out: [{ targets: ['USB 1'], channel: 0, bus: 1 }],
    },
  },
  'Feedback': {
    about: 'Buses are double-buffered, so feedback is a one-pass delay rather than a hang. Jack 1 flickers rather than the patch hanging.',
    patch: { gate_ports: [{ port: 1, dir: 'out', bus: 0 }], nodes: [{ algo: 'NOT', in: [0], out: [0] }] },
  },
};
