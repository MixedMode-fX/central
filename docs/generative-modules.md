# Five modules for generative music

An audit of the algorithms in `src/algorithm/`, read against one
question — *what would this module need before it could be left running on its
own and be worth listening to?* — and five proposals that answer it.

> **All six are built.** `CvToNote`, `CvToGate`, `Turing`, `Harmony`,
> `Automaton` and `NoteDelay` are algorithm ids 31..36, with 85 tests of their
> own. The README's [Generative
> Algorithms](../README.md#generative-algorithms) section is the user-facing
> description; this document is kept as the argument for why they exist and
> what was rejected. Where the build disagreed with the proposal, the reason
> is recorded in "What changed in the building" at the end.

The short version: the graph, the buses, the key and the note-off ledger are
already the right substrate for generative music, and the algorithm table is
missing four specific things. One of the four is a hole in the architecture
rather than a missing feature.

## Contents

1. Where the module stands
2. Four gaps
3. The five proposals
4. A suggested order of work
5. Considered and not proposed
6. Sources

---

## 1. Where the module stands

The thirty algorithms, grouped by what they do for a patch nobody is
touching:

| | |
|---|---|
| **Make time** | `ClockDiv`, `Metronome` |
| **Make events** | `StepSequencer`, `EuclidianSequencer`, `RandomSequencer`, `NoteSequencer`, `PolySequencer`, `DrumSeqGate`, `DrumSeqMidi`, `GateToNote` |
| **Change events** | `Transpose`, `Arpeggiator`, `Chord`, `NoteQuantise`, `NotePriority`, `VelocityCurve`, `Probability` |
| **Make control signals** | `Lfo`, `SampleHold`, `Slew`, `MidiToCV` |
| **Decide** | seven logic gates, `Sustain`, `GateHold` |

That is a strong list, and several of the decisions in it are worth more for
generative music than they look:

- **A step stores a scale degree, never a pitch.** Moving the root transposes
  a pattern and keeps it in key; changing the scale gives the same pattern a
  different character. Both are one-parameter operations on a pattern nobody
  has to re-enter.
- **`Chord`'s intervals are steps of the scale**, so `0 2 4` is major on I,
  minor on ii and diminished on vii&deg; with no chord-quality parameter
  anywhere. Chord quality already falls out of the key for free.
- **`Chord` and gates play with nothing patched in**, so a patch can start
  itself.
- **The note-off ledger.** A node releases what it sent, never what it would
  send now. Every generative idea below survives root changes, key changes,
  parameter edits and patch swaps because of this one invariant.

The gaps are not in the substrate. They are four missing shapes in the table.

## 2. Four gaps

### 2.1 Randomness here has no memory

Every random source in the module is all-or-nothing. `RandomSequencer` shreds
a whole new pattern. `SampleHold` gives a fresh number on every trigger.
`Probability` flips a memoryless coin per step. `StepEngine`'s `SEQ_RANDOM`
and `SEQ_BROWNIAN` walk a pattern somebody already programmed.

What is missing is the middle of the axis: a source that **repeats, and
mutates slowly** — a loop that survives for eight bars and then changes one
step. This is the single most-used idea in generative modular. Music Thing's
Turing Machine puts one knob on it (locked &rarr; slipping &rarr; random);
Mutable's Marbles calls the same control D&Eacute;J&Agrave; VU, "the
probability of recycling random decisions and values from the past". The
module has no point on that axis at all.

### 2.2 The patch has a key and no harmony

`GlobalSettings` carries one scale and one root, and nothing in the firmware
ever moves them. A generative patch here plays one chord for as long as it
runs, which is the most reliable way to make twenty minutes of generative
music boring.

The README already names the missing patch — "a sequenced root walking through
the chords of one key ... is a chord progression" — and then leaves the roots
to be programmed by hand, which is precisely the part that must not be hand
programmed. Everything downstream of a root is already built: `Chord` voices
it in key, the note sequencers take it on an inlet, `NoteQuantise` snaps to
it. There is no node that *generates* one.

### 2.3 Nothing reads a CV bus into a note or a gate

This is the architectural one. `Lfo`, `SampleHold`, `Slew` and `MidiToCV`
write a CV bus; `SampleHold` and `Slew` read one back; and `cv_read` outside
those appears only in `ModMatrix` and the console. There is no algorithm
anywhere that turns a control signal into a gate or into a note.

So a modulator in this module can move a **parameter** and can do nothing
else. It cannot play a note. It cannot fire a trigger. The first patch anyone
tries in generative modular — a slow, complex modulation source through a
quantiser is a melody — is not expressible, and neither is its rhythmic twin,
a comparator turning a wobbling voltage into an irregular but not random
clock.

It also blocks work already on the roadmap. `CvInPort` is listed under "Still
open" as the thing that would make "every external voltage a modulation
source". It would land in the same closed loop: an external voltage would
reach parameters and never reach a note.

### 2.4 Everything lands on the same grid

Every node is advanced by an edge, and in practice every edge in a patch
descends from one `ClockDiv` or `Metronome`. That is deliberate and it is what
makes the module tight. It also means a patch has exactly one rhythmic
surface, and nothing can deliberately place an event *between* the grid lines
and stay musical.

The whole ambient tradition is built on the opposite. Eno's *Music for
Airports* is seven tape loops of incommensurable length — 23&frac12;s,
25&#8542;s, 29&#8543;s — that re-align once every twenty-seven days. Reich's
phasing is the same idea. The module can build two sequencers of length 16 and
12 (and `DrumSeqGate`'s per-lane lengths make that easy), which is polyrhythm
on the grid; it has nothing that gets off the grid on purpose.

---

## 3. The five proposals

Each is an ordinary `Node` with an `AlgorithmDescriptor`, appended to
`AlgorithmId` (appended, never renumbered), with named ports, a parameter
block described the way `param.h` requires, and the existing ledger and key
machinery underneath. Nothing here needs a change to the bus model, the patch
format or the editor protocol.

---

### 3.1 `Harmony` — a chord progression the whole patch follows

**Closes gap 2.2.** The biggest musical jump of the five.

> **Superseded in part.** The five transition tables argued for here shipped
> and were later replaced by weights computed from the scale. The argument for
> *why a walk has to be weighted at all* stands; the argument for weighting it
> with a table per genre did not survive contact with a key the tables were
> not written for. See [harmony.md](harmony.md) §6.1.

A weighted random walk over the degrees of the key, one chord per advance
edge. Not a uniform walk — a **functional** one: a 7&times;7 transition table
biased the way tonal music actually moves, which is the same first-order
Markov model the harmonic-generation literature uses, at a size that fits in
flash.

```
in  0  gate  advance    one chord per rising edge (a Metronome at "1 bar")
in  1  gate  reset      optional; the next advance plays the tonic
out 0  note  root       the chord root, held until the next chord
out 1  cv    degree     optional; the degree over full scale, for the matrix
```

| param | what it does |
|---|---|
| `style` | `pop`, `modal`, `jazz`, `walk`, `pedal` &mdash; **removed.** The five tables were replaced by a walk computed from the scale, and the parameter went with them; see [harmony.md](harmony.md) and `src/midi/root_motion.h` |
| `phrase` | chords per phrase, 2..16 |
| `cadence` | how strongly the end of a phrase is pulled to the tonic |
| `motion` | scales the off-diagonal weights: at 0 it sits on I, at 100 the table is used as written |
| `loop` | keep the first phrase it finds and repeat it, instead of walking for ever |
| `octave` | the pitch the root is emitted at |
| `scale` | 0 follows the module's key, as everywhere else |

**Why it fits this module and not another one.** Because `Chord`'s intervals
are scale steps, `Harmony` only has to emit a *root* — the quality of each
chord is already correct by construction. A minor ii and a diminished vii&deg;
come out of a node that knows nothing about chord quality. No other module in
this list gets that much for free from a decision already made.

`loop` is worth arguing for separately: a walk that never repeats is drifting,
not composing. Generating four bars and then repeating them is the difference
between wandering and a song form, and it is one bool.

**Cost.** Five styles &times; 49 bytes of weights is 245 bytes of flash. State
is a cursor, a phrase counter, an RNG and a `SoundingNotes` ledger for the one
held root.

**The patch it unlocks.** `Metronome (1 bar) &rarr; Harmony &rarr; Chord &rarr;
MIDI out` is a self-playing chord progression in three nodes with nothing
patched in. Separately, `Harmony.root &rarr; NoteSequencer.root` puts a pattern
somebody wrote into a progression *without touching the pattern* — one cable,
because degrees are already stored rather than pitches.

---

### 3.2 `Turing` — the loop-to-random axis

**Closes gap 2.1.**

An N-bit shift register clocked by an advance edge. The bit that falls off the
end is fed back to the front — and with probability `chaos` it is inverted on
the way. At `chaos` 0 the loop repeats for ever; at 50 every bit is a coin
flip and nothing ever repeats; in between the loop survives and mutates, one
step at a time.

```
in  0  gate  advance    shift one bit
in  1  gate  reset      optional; back to the stored seed
out 0  gate  pulse      the bit that just fell off: a looping, mutating rhythm
out 1  cv    cv         the top `bits` of the register as a level
```

| param | what it does |
|---|---|
| `length` | 2..32, the loop length. Not a power of two against a divider is free polyrhythm |
| `chaos` | 0 locked, 50 fully random, in between slipping |
| `bits` | 1..8: how many bits the CV reads. 1 bit is two levels, 8 is smooth |
| `write` | force the incoming bit low or high &mdash; the hand-operated escape hatch that clears or fills a pattern while it runs |
| `seed` | `PARAM_BITFIELD`, the eight bits reset returns to |

**Why two outlets and not one.** Because the register is a rhythm and a
melody at the same time, and driving both from *one* register is the point:
the pitch and the rhythm mutate together, so they stay related. Two
independent random sources sound like two random processes; one register
sounds like a part.

**Cost.** A `uint32_t`, a shift, one RNG call per edge. About 120 lines, and it
belongs beside the modulators.

---

### 3.3 `Quantiser`, with `Comparator` beside it — let the CV bus reach the music

**Closes gap 2.3.** The smallest change in this document and the one with the
largest multiplier, because it adds no generative idea of its own and instead
makes every idea already in the module reach the output.

`Quantiser` reads a CV, maps it across `range` octaves from `root`, snaps it
into the key with `scale_quantise()` — already written, already tested — and
emits a note, releasing the previous one from the ledger.

```
in  0  cv    pitch      the signal to quantise
in  1  gate  trigger    optional; in `trigger` mode, when to read it
in  2  cv    velocity   optional
out 0  note  note
```

| param | what it does |
|---|---|
| `root` | the pitch the bottom of the range sits on |
| `range` | 1..8 octaves across full scale |
| `scale` | 0 follows the module's key |
| `mode` | `track` (re-emit whenever the quantised pitch changes) or `trigger` (only on an edge) |
| `gate` | note length in ms; 0 holds until the pitch changes |
| `channel` | |

`Comparator` is the same bridge in the other domain and is forty lines: CV in,
gate out when the signal crosses `threshold`, with `hysteresis` so a noisy
signal does not chatter, and an `invert`. Neither half is much use without the
other — a patch that can turn a voltage into a note but not into the trigger
that plays it is still stuck — so they ship together.

**What it turns on, with no other work:**

- `Lfo` becomes a melody generator. Two LFOs summed on one CV bus (fan-in is
  already a sum) become a melody that does not repeat for hours, which is the
  standard recipe and is currently inexpressible.
- `SampleHold` becomes a random-note source; its `steps` parameter, which
  already quantises the held level, becomes a musical control.
- `Slew` becomes portamento.
- `Turing` (3.2) becomes a looping, mutating melody.
- `Comparator` on a slow LFO becomes an irregular clock that is not random.
- The modulation matrix stops being the only thing a CV bus can reach.
- **`CvInPort` becomes worth building.** An external voltage would reach notes
  and triggers, not only parameters.

---

### 3.4 `NoteDelay` — one line becomes an ensemble

**Closes gap 2.4.**

A note-bus delay that is also a canon generator. A note in is re-emitted
`repeats` times: each after `delay`, each transposed by `interval` **scale
steps** (so a canon at the third stays in key), each `decay` percent quieter,
each subject to `chance`.

```
in  0  note  note in
in  1  gate  clear      optional; drop everything scheduled and release it
out 0  note  note out   the input plus its repeats
```

| param | what it does |
|---|---|
| `delay` | a `MusicalDivision` + `MusicalFeel` &mdash; the same vocabulary `Metronome` and `Lfo` already offer, so 3/16 dotted is available and exact |
| `free` | a wall-clock delay in ms instead, for the deliberately unsynced case |
| `repeats` | 1..8 |
| `interval` | signed scale steps added per repeat |
| `decay` | velocity percent per repeat |
| `chance` | percent that each repeat happens at all |
| `spread` | per-repeat drift in ms, signed |

**`spread` is the whole point of the node.** At 0 the repeats are on the grid
and it is a musical delay. Above 0 each repeat walks a little further off the
grid, and the echoes of a four-note pattern stop lining up with each other —
which is Eno's incommensurable loops, expressed as one parameter on one node,
in a module where everything else is locked to one clock by construction.

**What it costs and what it must respect.** A ring of scheduled
`{MidiEvent, due}` entries, plus the ledger. `NOTE_QUEUE_DEPTH` is 32 events
per bus per pass, so the node caps what it emits in one pass and counts what
it dropped, the way `BusManager` already counts note overflows. `repeats`
being bounded rather than a feedback percentage is deliberate for the same
reason: the note count is knowable in advance.

**The patch.** A four-step `NoteSequencer` plus a `NoteDelay` at 3/16 with
`interval` = +2 scale steps is a two-part invention out of a four-note
pattern. With `spread` up it is an ambient wash. Neither needs a second
sequencer, a second voice or a second MIDI channel.

---

### 3.5 `Automaton` — rhythm that is neither periodic nor random

The rhythm family today is `StepSequencer` (you programmed it),
`EuclidianSequencer` (a formula: perfectly even and perfectly predictable),
`RandomSequencer` (no memory) and the drum sequencers (you programmed them).
All four sit at one end of the axis or the other. Nothing is in the middle: a
pattern with structure, that repeats motifs, and never quite repeats itself.

A one-dimensional cellular automaton lives exactly there, and is famously
cheap. `DRUM_SEQ_LANES = 8` cells in a row; on each advance edge every cell
becomes `rule[(left << 2) | (self << 1) | right]` — a Wolfram elementary rule,
eight bits, one parameter byte. Rule 90 draws Sierpinski triangles (sparse,
self-similar, unreasonably musical); rule 110 is the structured-but-chaotic
one that is Turing complete; 30 is noise; 150 is dense and symmetric.

```
in  0  gate  advance
in  1  gate  reseed     optional; back to `seed`
out 0..7  gate  cell 1..8    one per cell, unpatched outlets simply unused
out 8     cv    row          the row as a number, so it modulates as well
```

| param | what it does |
|---|---|
| `rule` | 0..255 |
| `seed` | `PARAM_BITFIELD` &mdash; eight bits, the shape the kind already describes |
| `wrap` | cell 0's left neighbour is cell 7 (a ring, so it loops) or dead (so it decays) |
| `revive` | reseed automatically after N advances with nothing alive |
| `probability` | as the other sequencers have it |

`revive` is the one thing the mathematics does not give you: an automaton that
reaches all-zeros is dead and stays dead, and a drum machine that stops for
ever is a bug however correct the rule is.

**Why eight cells and not eight sequencers.** Eight `EuclidianSequencer`s give
eight unrelated patterns. An automaton's lanes are *neighbours*: what happens
on the kick propagates to the snare two steps later, because that is what the
rule does. Related lanes are a rhythm section; unrelated ones are eight
sequencers in a rack.

**Cost.** One byte of cells, one byte of rule, a table lookup. Under a hundred
lines — the cheapest node in this document and the least like anything already
in the table. It reuses `DrumSeqGate`'s multi-outlet shape exactly, including
leaving unused outlets at `NO_BUS`, which the validator already permits.

---

## 4. A suggested order of work

1. **`Quantiser` + `Comparator`.** Smallest, no new concepts, and it unblocks
   everything else — including `Turing`, whose CV outlet is inert without it,
   and `CvInPort`, which is already on the roadmap.
2. **`Turing`.** The missing axis, and it feeds the quantiser the moment both
   exist.
3. **`Harmony`.** The biggest musical jump, and it is worth having the first
   two to hear it against.
4. **`Automaton`.** Cheap and distinctive; independent of the other four.
5. **`NoteDelay`.** The most state and the most care around the ledger and
   `NOTE_QUEUE_DEPTH`. Worth doing once the rest have proved themselves on
   hardware.

After 1 and 2, this patch exists and has no input:

```
Metronome 1/8 ─┬─> Turing ──cv──> Quantiser ──> NoteQuantise ──> MIDI out
               └─> Turing.pulse ──> DrumSeqGate.advance
```

After 3, it is in a key that moves. After 5, it is an ensemble.

## 5. Considered and not proposed

- **A Grids-style drum map.** Interpolating an X/Y map of drum patterns
  trained on a corpus is a lovely control surface, and the corpus is megabytes
  and a data pipeline. `Automaton` reaches a lot of the same
  sparse-skeleton-to-busy-pattern territory for a hundredth of the cost.
- **Trig conditions** (Elektron's `1:4`, `fill`, `prev`). Genuinely useful and
  genuinely *not a node*: it is a field on every existing sequencer, so it
  belongs on `StepEngine` and in each sequencer's step block, not in the
  algorithm table.
- **A voice allocator / round-robin distributor.** Needed for polyphony,
  orthogonal to generation.
- **Ratcheting / burst.** Blocked on the same sub-step duration estimate
  `NoteSequencer` already flags as unproven on hardware; building on it twice
  before it is proved once is the wrong order.
- **L-systems.** A rewrite grammar is a beautiful generator and does not fit a
  336-byte parameter block or an editor made of knobs and lists.

## 6. Sources

- Music Thing Modular, [Turing Machine](https://www.musicthing.co.uk/Turing-Machine/) — the locked / slipping / random control.
- Mutable Instruments, [Marbles manual](https://pichenettes.github.io/mutable-instruments-documentation/modules/marbles/manual/) — D&Eacute;J&Agrave; VU as "the probability of recycling random decisions from the past".
- Mutable Instruments, [Grids manual](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/manual/) — the drum map and per-channel density.
- [Generative Jazz Chord Progressions: A Statistical Approach to Harmonic Creativity](https://www.mdpi.com/2078-2489/16/6/504) — first-order Markov chord transition models.
- [Realtime Generation of Harmonic Progressions Using Constrained Markov Selection](https://www.researchgate.net/publication/228610084_Realtime_Generation_of_Harmonic_Progressions_Using_Constrained_Markov_Selection) — constraining a harmonic walk in real time.
- Reverb Machine, [Deconstructing Brian Eno's Music for Airports](https://reverbmachine.com/blog/deconstructing-brian-eno-music-for-airports/) — incommensurable loop lengths.
- [Elementary cellular automaton](https://en.wikipedia.org/wiki/Elementary_cellular_automaton) and [Rule 110](https://en.wikipedia.org/wiki/Rule_110) — the rule numbering and the edge-of-chaos behaviour.
- Music Thing Modular Workshop Computer, [Cellular Automata Sequencer](https://computer.musicthing.co.uk/programs/19-ca-sequencer/) — an existing gate-and-CV automaton in a Eurorack context.
- Loopop, [Create Brian Eno style generative music](https://loopopmusic.com/create-brian-eno-style-generative-music-20-ideas-and-tools-for-ableton-eurorack-dawless-and-vcv) and MacProVideo, [Making Generative Music With Eurorack Synths](https://www.macprovideo.com/article/midi/making-generative-music-with-eurorack-synths) — the modulation-source-through-a-quantiser patch, and blending sequences of different lengths.


## 7. What changed in the building

Five things in the proposal above turned out to be wrong or under-specified,
and the code follows the corrected version rather than this document.

- **`CvToNote` and `CvToGate`, not `Quantiser` and `Comparator`.** The README
  argues at length that `NoteQuantise` is called that, and not `Quantise`,
  because the module quantises two unrelated things. The same argument applies
  here, and the module already had the naming for it: `GateToNote` is a
  gate-to-note bridge, so these are the CV ones. "Quantiser" and "comparator"
  are what the summaries say, because that is what a user searches for.

- **`gravity`, not `motion`.** A stored 0 means the descriptor's default
  everywhere in this module, so a `motion` parameter whose useful default was
  100 could never be *saved* at 0 — the setting that pins the walk to the
  tonic would have been unreachable from a preset. Naming the parameter for
  the pull rather than for the movement puts the musical default at zero,
  where the format needs it. `cadence` pays a smaller version of the same
  price and keeps its default of 75; "no cadence at all" is 1.

- **`Automaton` has no CV outlet.** `MAX_OUT` is 8 and the eight lanes are the
  point. Raising it is a preset-format change to buy a feature `Turing`
  already provides better, so the row is eight gates and nothing else.

- **`NoteDelay`'s `spread` is a percentage, not milliseconds.** Milliseconds
  cannot mean the same thing in both timing modes, and the synced mode counts
  subticks. As a percentage of the delay it works in either unit and at any
  tempo — and it made the *shape* right as well: each gap is a percentage
  longer than the one before, so the total grows with the square of the
  repeat, which is what walking off the grid actually is. A flat offset would
  only have been a slower grid.

- **Two defects only showed up when the patch was actually played.** Sixty
  seconds of the worked example, run through the WebAssembly build and every
  note checked against the key, found both. `NoteDelay` had no root of its
  own, so a node that named its own scale transposed in that scale's interval
  pattern rooted on C — a third away in the wrong key. And with `dry` passing
  the input through, it forwarded notes it did not own, so a patch swap
  released the echoes and left the copy sounding: the note-off from the node
  upstream lands on an intermediate bus nothing reads any more. Both are the
  kind of thing a unit test of one node cannot see, because both are about
  what the node is *connected to*.

- **`MidiToCV` landed on `main` while this branch was being written**, and it
  is the other half of the CV story rather than a collision of ideas: that
  node sends a note stream out as pitch, gate and velocity, `CvToNote` brings
  a control signal back as notes. What it *did* collide with was the id — it
  took 30, so these six moved to 31..36, because an id is preset format and
  the one that shipped first keeps it. It also means the gap in §2.3 is
  stated more carefully than it was: `MidiToCV` writes a CV bus, so the
  finding was never "three descriptors mention CV", it was that nothing
  anywhere *read* one into a note or a gate.

- **The node pool had to grow, and that was not free.** The registry passed 32
  algorithms, so a patch could no longer hold one of every algorithm. `N_NODE`
  is 40 — and the ceiling is the NRPN address space rather than memory, so the
  block bases moved and the protocol version went to 4. Two other things
  surfaced with it: the emulator's 4 KB SysEx buffer silently dropped five of
  the thirty-six algorithm records (raised to 16 KB, and the seam now fails on
  a drop rather than handing back a truncated reply), and
  `semitone_to_scale_degree`'s comment claimed an offset outside the scale
  resolves to the degree *below* it when it resolves to the next note above.
