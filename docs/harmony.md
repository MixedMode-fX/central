# Generative harmony

A design note on what a harmony module family for the MMMC should be, why the
circle of fifths is the right object to build it on, and what has to be added
to the firmware to get there.

> **Status.** `Harmony` (algorithm id 34) shipped on `main` while this was
> being written, and it answers §1's root-motion gap — see
> [generative-modules.md](generative-modules.md) for its own argument. What
> §2 to §5 say about the circle of fifths is the theory underneath it and is
> unchanged; §6 is now the four things still missing, and the reasoning about
> where `Harmony` and this document disagree is kept in §6.1.
>
> `Voicer`, `Mirror` and `Tonnetz` (ids 37..39) and the two `Chord` changes
> are built, with tests. §8 records the order they were built in and what the
> building changed.

The short version: **the module already has the top and the bottom of the
harmonic stack and nothing in the middle.** It knows what key it is in, and
it knows how to play a chord and how to arpeggiate one. It has no opinion at
all about *which* chord comes next, or about *where the notes of that chord
should sit*. Those two are the whole of what makes a progression sound
composed rather than shuffled, and they are the two layers that need memory
of the previous chord — which is exactly why neither of them fell out of the
stateless modifiers we have.

---

## 1. The harmonic stack, and the hole in the middle

| Layer | The question it answers | What answers it today |
|---|---|---|
| Key | which twelve-bit pitch collection, rooted where | `global_scale`, plus a per-node override (`src/midi/scale.h`) |
| Root motion | which degree is the chord on now | `Harmony` — a weighted walk over the degrees |
| Quality | how the chord is stacked on that degree | `Chord`, partially — the intervals are typed in by hand |
| **Voicing** | where the notes of that chord actually sit | **nothing** — `Chord` always stacks upward from the root |
| Figuration | in what order and rhythm they are played | `Arpeggiator`, the note sequencers |

When this was written both bold rows were empty. `Harmony` filled the first
of them, and it filled it the way §2 argues for: **it emits a root and
nothing else**, because `Chord`'s intervals are already scale steps and the
quality of each chord therefore falls out of the degree it lands on. The
second row is still empty, and it is the one a fifth-walk makes most audible
— root-position triads a fifth apart leap, and the same two chords voice-led
share a note and move one voice.

The two layers were missing for the same structural reason. Every
harmonic node in the tree so far is a function of the note in front of it:
`NoteQuantise` snaps a pitch, `Transpose` shifts it, `Chord` stacks on it.
Root motion and voicing are functions of the note in front of it *and the
chord before it*. A node with that memory is a different kind of node, and
the module has a good place for it already — `StepEngine` nodes hold a
cursor and take an advance edge, and that is precisely the shape a
progression generator wants.

`NoteSequencer` looks as if it fills the root-motion hole and does not. It
stores degrees, so a hand-written I–V–vi–IV is possible today by typing four
degrees into four steps. What it cannot do is *generate* one: a sequencer
plays back what a person entered, and generative harmony is about a rule that
produces plausible root motion without one.

---

## 2. Root motion is scale-degree arithmetic

The single idea everything below rests on:

> **A fifth is four scale steps. Motion by fifths is addition modulo the size
> of the scale, and addition modulo the scale can never leave the key.**

In a seven-note scale, up a fifth is `+4` degrees and down a fifth is `-4`,
which is `+3` mod 7 — the same chord, an octave apart, and the octave is
`scale_degree_to_semitone`'s problem, not ours. So the whole circle of fifths,
restricted to one key, is one line of arithmetic:

```
degree = (degree + 3) % 7        // descending fifths
```

Iterate it from the tonic and you get

```
I → IV → viiº → iii → vi → ii → V → I
0    3     6     2     5    1    4    0
```

which is the diatonic circle-of-fifths progression, the strongest single
engine in tonal music, produced by a modulo add on a byte.

Three things fall out of doing it in degree space rather than in semitones,
and each of them is a bug avoided rather than a feature added:

**It stays in key by construction.** No quantiser downstream, no accidentals
to explain, no "why did that chord go outside the scale". The alternative —
`+7 semitones, then snap` — produces B♭ in C major on the way round and then
snaps it to B or A depending on which way the tie breaks. Degrees never have
that question.

**The chord qualities are correct for free.** `Chord` already voices its
intervals as scale steps, so `0 2 4` on degree 0 of a major scale is major,
on degree 1 minor, and on degree 6 diminished. Nothing has to know that the
seventh degree is the odd one out; stacking thirds in a mask *is* the
definition of the chord quality. This is already true in the firmware today
and is the reason the progression node needs to emit only a root.

**It generalises to any mask.** The interesting version of "the circle of
fifths" is "the cycle generated by adding *k* degrees", and it is a full cycle
over every degree exactly when `gcd(k, n) = 1`. For a seven-note scale that is
every *k* except 0 and 7:

| *k* | what it is | the cycle it generates in a major scale |
|---|---|---|
| 1 | stepwise | I ii iii IV V vi viiº |
| 2 | chain of thirds — mediant motion | I iii V viiº ii IV vi |
| 3 | descending fifths / ascending fourths | I IV viiº iii vi ii V |
| 4 | ascending fifths | I V ii vi iii viiº IV |
| 5 | descending thirds | I vi IV ii viiº V iii |
| 6 | descending steps | I viiº vi V IV iii ii |

That is a single one-byte parameter spanning stepwise motion, Romantic
mediant chains, and functional fifth motion, all of them in key, all of them
visiting every degree before repeating. In a five-note scale `k = 2` and
`k = 3` are each other's retrograde and there is no fifth to walk; in the
whole-tone scale (n = 6) nothing is coprime except 1 and 5, which is a fair
description of what whole-tone harmony is like. The node should clamp
`interval` to what the mask can actually generate and say so, rather than
offer a fifth in a scale that has none.

**And ii–V–I is a tail of the circle.** Degrees 1, 4, 0 are three consecutive
steps of the `k = 3` cycle ending on the tonic. Any cadence formula worth
having is "the last *m* steps of the circle before the phrase ends", which
means the cadence logic in §3 is not a special case bolted on — it is the
same arithmetic run backwards from the target.

---

## 3. From a cycle to a progression: gravity and a horizon

A pure cycle is a loop: seven chords and it repeats forever, which is a
Sequencer with extra steps. A pure random walk over the degrees is a wander:
every chord is a surprise and nothing is a destination. Neither is music.
What sits between them is two ideas, and they are cheap.

### 3.1 Gravity: a distribution over root motion, not over roots

Functional harmony is much better described by *how far the root moved* than
by *which degree it landed on* — that is the whole content of Meeùs' reading
of Rameau, and it happens to be the representation that costs one small table
and works in any scale. Motion is signed degrees; the defaults:

In a seven-note scale there are exactly six non-zero motions, because `+4` and
`−3` name the same move an octave apart. All six, with the weights I would
ship as defaults:

| Motion | Name | Class | Weight |
|---|---|---|---|
| +3 (≡ −4) | down a fifth, up a fourth | dominant | 8 |
| +2 | up a third | dominant | 4 |
| −2 | down a third | subdominant | 5 |
| −3 (≡ +4) | up a fifth | subdominant | 3 |
| +1 | up a step | either | 4 |
| −1 | down a step | either | 3 |
| 0 | repeat | — | 0 |

Down-a-fifth is the strongest move; up-a-fifth is its retrograde and is how
one *sets up* a dominant rather than resolves one; down-a-third is I–vi;
up-a-step is IV–V. The table is indexed by motion and is six bytes, so a
scale of a different size re-derives it rather than storing a second copy. A weighted draw from that table produces progressions that
sound like they were written, because the weights are what "sounds written"
means.

One control shapes it: **gravity**, 0 to 100. At 0 the distribution is flat —
every legal motion equally likely, which is the wander. At 100 the strongest
available motion always wins, which is the circle. Everything interesting is
in between, and the interpolation is a single exponent applied to the weights
before the draw, not a second table.

### 3.2 A horizon: the cadence is a deadline, not a chord

A progression without phrase structure has no shape, and phrase structure is
the cheapest thing here: a counter, a target, and a distance table.

- **Phrase** — chord changes per phrase, 0 (off) or 2..32.
- **Cadence** — what the phrase must land on: authentic (I, approached from
  V), half (stop on V), plagal (I from IV), deceptive (vi from V), none.

The mechanism is a distance table. Build it once, at construction, by a
breadth-first walk of the allowed motion set from the tonic outward: seven
entries, seven degrees, and it is rebuilt only when the scale or the motion
set changes. Then the rule is one comparison: **while the phrase has more
steps remaining than the current degree's distance to the cadence target, draw
freely from the weighted distribution; once they are equal, only motions that
reduce the distance are legal.** The progression wanders and then, without
anything being scheduled, arrives.

The cadence parameter names only the *last* move — authentic means the final
approach must be V–I, deceptive V–vi, plagal IV–I — and everything before it
is still an ordinary weighted draw under the horizon constraint. Which is why
ii–V–I emerges instead of being spelled: two steps out from the tonic, several
motions reduce the distance, but the fifth carries twice the weight of any of
them, so the fifth chain is what usually wins.

That is the part worth building for its own sake. Cadence-as-a-horizon is
what separates generative harmony that sounds composed from generative harmony
that sounds like a Markov chain, and it is a `uint8_t[7]` and one `if`.

### 3.3 Harmonic rhythm is not this node's business

Chords change on an advance edge, like every other pattern node in the module.
A `Metronome` at `1 bar` into the advance inlet is the normal case, a
`ClockDiv` is the odd one, and a Euclidean sequencer into it gives
non-isochronous harmonic rhythm for free. Nothing about this node needs a
tempo.

---

## 4. Modal harmony is the other axis, and it is the same circle

### 4.1 Brightness *is* the circle of fifths

Order the seven diatonic modes on one tonic from bright to dark and each step
flattens exactly one degree:

| Mode on C | Notes | Flattened from the mode above | Parent key |
|---|---|---|---|
| Lydian | C D E F♯ G A B | — | G |
| Ionian | C D E F G A B | F♯ → F | C |
| Mixolydian | C D E F G A B♭ | B → B♭ | F |
| Dorian | C D E♭ F G A B♭ | E → E♭ | B♭ |
| Aeolian | C D E♭ F G A♭ B♭ | A → A♭ | E♭ |
| Phrygian | C D♭ E♭ F G A♭ B♭ | D → D♭ | A♭ |
| Locrian | C D♭ E♭ F G♭ A♭ B♭ | G → G♭ | D♭ |

The notes being flattened, in order, are F♯ B E A D G — the circle of fourths.
The parent keys, in order, are G C F B♭ E♭ A♭ D♭ — the circle of fifths. **The
modal brightness axis and the circle of fifths are the same line read two
ways**, and that is why a single "brightness" control is not a simplification
of modal music but an accurate model of it: brightness −1 means "parent key
down a fifth", and every step changes exactly one note.

This is the strongest argument for a *CV-modulatable* brightness: a slow LFO
on brightness is a drifting modal wash where no single step ever moves more
than one semitone in one voice, which over a tonic drone is one of the most
reliably beautiful things in generative music. It is also the thing this
architecture makes hardest — see §6.6.

### 4.2 The dominant is what kills a mode

A modal passage stops being modal the moment it plays a V–I with a leading
tone, because the leading tone is only in Ionian and Lydian, and hearing it
re-hears the tonic as a major key. So a progression generator that only knows
the fifth-walk cannot do modal music: fifth-walking *manufactures* the
cadence it should be avoiding.

The rule is generic and needs no per-mode table. **If the mask contains a
semitone below the tonic (bit 11), the triad on the degree that carries it is
the mode-destroying chord.** In a modal setting, weight it to zero.

### 4.3 The characteristic tone, computed rather than tabulated

Modal harmony works by shuttling between the tonic and the chord that carries
the mode's characteristic tone — the note that distinguishes it from its
parallel major or minor. Dorian's ♮6, Mixolydian's ♭7, Lydian's ♯4,
Phrygian's ♭2. That list is also computable from the mask alone:

1. Pick the reference: natural minor if bit 3 is set (a minor third),
   major otherwise.
2. XOR the mask with the reference. The set bits are the characteristic
   tones.
3. The characteristic *chord* is the triad whose quality differs between the
   mask and the reference — build `0 2 4` in both and compare. In Dorian
   that is IV (major, where Aeolian has it minor); in Mixolydian ♭VII; in
   Lydian II.

Three lines, no table, and it works for a user-defined mask the firmware has
never seen — which matters, because the scale is twelve bits precisely so
that scales the firmware does not name are still playable.

### 4.4 One knob from tonal to modal

Two motion distributions — the weighted fifth-gravity table of §3.1, and a
modal table that weights the tonic and the characteristic chord heavily,
plagal motion moderately, and the dominant at zero — and one parameter that
crossfades between them. At one end, functional cadential harmony; at the
other, a modal shuttle that never resolves; in between, the ambiguous
territory that most good generative harmony actually lives in. One byte,
because the two tables are indexed the same way.

---

## 5. The chromatic complements

Everything above stays in key, which is what was asked for. Three transforms
are worth having anyway, because each one is a different *reading of the same
circle*, and each is cheap.

### 5.1 The Tonnetz: the circle in two dimensions

Three transforms on a triad, each moving exactly one voice by one or two
semitones:

- **P** (parallel) — C major ↔ C minor. The third moves a semitone.
- **L** (leading-tone exchange) — C major ↔ E minor. The root moves down a
  semitone.
- **R** (relative) — C major ↔ A minor. The fifth moves up a tone.

Alternate two of them and you get a cycle, and the three cycles are the three
symmetric divisions of the octave:

| Alternation | Cycle | Roots |
|---|---|---|
| L, R | fifths | C Am F Dm B♭ … |
| P, L | major thirds (hexatonic) | C Cm A♭ A♭m E Em C |
| P, R | minor thirds (octatonic) | C Cm E♭ E♭m G♭ … |

**The circle of fifths is the L–R cycle of the Tonnetz.** Which is the whole
justification for putting a neo-Riemannian node next to a diatonic one: it is
not a different idea, it is the same object with the other two axes exposed.
An L/R-only walk stays diatonic; letting P in is where the Romantic,
film-score chromaticism comes from, and every step is still a single voice
moving a semitone, so it never sounds like a jump.

### 5.2 Negative harmony is the circle reflected

Reflect pitch classes about the axis midway between the tonic and the
dominant. Relative to the tonic that is `x ↦ (7 − x) mod 12`, and the whole
theory is that one expression:

| Chord in C | Reflected | What it became |
|---|---|---|
| C E G (I) | C E♭ G | i |
| G B D (V) | F A♭ C | iv |
| F A C (IV) | G B♭ D | v |

The dominant becomes the subdominant minor and the subdominant becomes the
minor dominant: **the reflection exchanges the two halves of the circle of
fifths about the tonic.** Applied to a progression it produces that
progression's shadow, which is a genuinely different piece of music derived
for free from one that already works — the ideal generative move.

### 5.3 Tonicization, for one chromatic note at a time

The controlled way out of the key: before moving to the next root, make the
current chord the dominant of it — raise its third to a major third, and
optionally flatten its seventh. Both are semitone edits to notes that are
already being emitted, both are undone by the next chord, and a probability
parameter turns it from a rule into a colour. That is secondary dominants,
which is most of the chromaticism in tonal music, for one `if` per voice.

---

## 6. What to build

Four things, in the order I would build them. All three new nodes are
`CATEGORY_MIDI`, which is where `Harmony` and `Chord` already are: an
algorithm's category is the shelf a host lists it on, and "notes: what
happens to them on the way past" is a true description of all three. A
`CATEGORY_HARMONY` would split the harmony family across two shelves to gain
nothing.

### 6.1 `Harmony` — the root-motion generator

This section proposed a node called `Progression`. `Harmony` was built
instead, on the same argument and with the same outlet: **a root, not a
chord**, because duplicating `Chord`'s diatonic voicing would be two places
to fix a bug in. It has the advance and reset inlets, the phrase counter, the
cadence probability, a `gravity` control and a degree outlet on the CV bus.

**It shipped with five 7 × 7 tables of weights, one per named style. They and
the parameter that selected them are gone, replaced by the computation §2 and
§3.1 argued for.** The
reason is the one this document gave and then failed to insist on: a table is
written for seven degrees, so a pentatonic key used five columns tuned for
diatonic function and a key nobody anticipated got numbers that meant nothing.
A table also says what a genre does rather than why, and the only progressions
reachable are the ones somebody typed — there is no room in it for an
accident, which for a generative node is the whole point.

What replaced it is not quite §3.1 either, and the difference is worth
recording. §3.1 proposed weighting motion in **scale steps**; the shipped rule
weighs it in **semitones** (`src/midi/root_motion.h`). That is strictly
better, and the reason is the case §2 got wrong: "down a fifth is three
degrees up" is true in a major scale and false everywhere else, and even
inside a major scale it is false once — IV to vii° is three degrees up and a
*diminished* fifth, which is exactly why it is the weak link in the diatonic
circle. Measuring the interval gets that for free, and gets a pentatonic key's
real fifths for free with it. The `k`-cycle table in §2 is a nice piece of
number theory and the wrong model.

Four controls, each one fact about the two chords:

| | |
|---|---|
| `fifths` | how far the root moved and which way round the circle — §3.1's dominant/subdominant axis, as one knob |
| `smooth` | a crossfade between "what the interval is worth" and "what the two chords share". Not §3.1's bonus on common tones: a fifth outweighs a third three to one, so a bonus can only nudge and a crossfade can actually arrive at Romantic mediant motion |
| `leading` | §4.2's rule, computed. The triad carrying the semitone below the tonic, weighted up for cadences or down for modality — and inert by itself in a mode that has no leading tone, which is what makes it one control rather than a per-mode table |
| `spread` | temperature, as §3.1's `gravity` was meant to be before the name was taken. Sharpens toward a loop, flattens toward uniform |

**Its cadence is still a probability, not a horizon.** `cadence` is the chance
the phrase's last chord is the tonic, which is one number and lands where
§3.2 lands most of the time. The distance-table horizon buys the *approach* —
arriving at the tonic through the dominant rather than jumping to it — and
that remains a refinement of a shipped node rather than a missing one.

**What §4 still does not have** is the brightness axis (§4.1) and the
characteristic-tone shuttle (§4.3). `leading` covers the half of modal harmony
that is about avoiding the cadence; it does not know what a mode's
characteristic tone *is*, so it cannot favour the chord that carries it. That
is the one piece of §4 left, and it is now a small one: the helpers are pure
functions of a mask and would sit beside the ones in `root_motion.h`.

**`drift`** is not in this document at all, and is the most useful thing in
the node. A loop repeats; a loop that redraws one chord now and then and
*keeps* it is how an accident becomes a decision. It is five lines and it is
what `spread` is for in the first place.

### 6.2 `Voicer` — where the notes actually sit

Notes in, notes out. Holds the chord currently sounding and, when it changes,
places the new one so that the voices move as little as possible: try each
inversion and octave placement inside a configured range, keep the one with
the smallest total motion from the previous voicing.

The feature that earns it is **common-tone retention**: a note the new chord
shares with the old one is not released and re-struck, it is simply held. Two
triads a fifth apart share one tone and two a third apart share two, so on a
fifth-walk this is the difference between a chord change and a chord *moving*.
`SoundingNotes` supports it as it stands, because a record is keyed on the
source note rather than on the emitted one — key on a voice index instead and
"voice 2 did not move" is expressible without touching the ledger's
invariants.

| | |
|---|---|
| Inlets | 0 notes in (note, required) |
| Outlets | 0 notes out (note) |
| Params | `mode` (closest, root position, drop 2, open, spread) · `low` (pitch) · `high` (pitch) · `hold common` (bool) · `max voices` |

### 6.3 `Mirror` — negative harmony

§5.2. Notes in, notes out, `axis` (pitch class pair, defaulting to the key's
tonic–dominant), `amount` as a probability so it can be a colour rather than a
switch, and `in key` to re-quantise the reflection. Perhaps a hundred lines,
including the ledger discipline, and it changes the character of any patch it
is dropped into.

### 6.4 `Tonnetz` — the chromatic walker

§5.1. A generator like `Harmony` rather than a modifier, because P/L/R are
defined on a triad the node should own rather than on an arbitrary set of
notes it would have to parse back into one. Advance and reset inlets, a triad
out, `cycle` (LR, PL, PR, free PLR), `deviation` (0..100), `diatonic` (bool,
which drops P), and a root.

### 6.5 `Chord`: quality presets, and one fix

Two small changes:

**A `quality` parameter.** Six typed intervals is the general case and a
terrible default. In degree space the useful stacks are all short: triad
`0 2 4`, seventh `0 2 4 6`, ninth `0 2 4 6 8`, sixth `0 2 4 5`, sus2
`0 1 4`, sus4 `0 3 4`, quartal `0 3 6`, shell `0 4 6`. An enum that fills the
interval array leaves the hand-typed case exactly where it is, as
`quality = custom`.

**A re-strike on a repeated root.** `Chord::play_free` compares the wanted
root against `voiced` and returns early when they match, so a progression that
plays the same degree twice in a row produces no second chord — and `Harmony`
repeats a degree whenever its style or its `gravity` says so, so one chord of
the phrase simply does not sound.

This section called that a bug and it is not one: there is a test that asserts
it, and the reading behind it is right. A held chord that re-struck itself
every time a sequencer resent the note it is already playing is a chord nobody
could drone on, and droning is what a self-playing `Chord` is for. Both
readings are wanted, so it is `retrigger`, defaulting to the behaviour that
shipped.

### 6.6 The one thing that does not fit, and why to defer it

A **key walk** — modulating the whole patch by a fifth every sixteen bars, or
CV over modal brightness as §4.1 argues for — cannot be a node today. The key
is `global_scale`, control-plane state whose single writer is
`PatchManager::push_globals()`, and that is a good rule: a scale threaded
through `BusManager` would make the key a signal, which it is not.

The partial answer available now is that a root inlet outranks the global key
on every node that has one, so `Harmony`'s root outlet already moves a
whole subgraph — what it cannot move is the *mask*, so it tonicizes but does
not modulate.

Since this was written the key has gained a **register** — `root_octave` in
`GlobalSettings`, a root that is a note rather than only a pitch class, so a
node with an absolute root follows the module's octave as well as its pitch
class (`src/midi/global_scale.h`). That closes the half of the gap that was
about *where* rather than *which*, and the note sequencers stopped being the
exception to the key's root. It does not change this section: the register is
control-plane state with the same single writer, so a node still cannot move
it, and a key walk is still the same open decision about who owns the key. The honest fix is a control-plane path: a node requesting a
global-key change between passes, the way `set_param` writes are enqueued and
applied. It is a real design decision about who owns the key, it deserves its
own argument, and none of the five nodes above need it.

---

## 7. What it costs

Measured, not estimated.

| | |
|---|---|
| `Voicer` state | 184 bytes — the held chord, the previous voicing, the ledger |
| `Mirror` state | 128 bytes |
| `Tonnetz` state | 136 bytes |
| `Chord` state | 136 bytes, unchanged: `quality` and `retrigger` are two bytes inside the padding it already had |
| Pool slot | `NODE_SLOT_SIZE` is 640, so the largest of the three uses under a third of one |
| Params | 6, 6 and 9 against `N_PARAM` = 336 |
| Algorithms | 36 to 39, against `N_NODE` = 40 — a patch can still hold one of every algorithm, which is a test this repository runs |
| Preset format | three appended `AlgorithmId`s and two appended `Chord` parameters. Nothing renumbered, and a preset that never mentions the new parameters plays exactly what it stored |

No new category: all three are `CATEGORY_MIDI`, where `Harmony` and `Chord`
already are. A `CATEGORY_HARMONY` would split the family across two shelves
of the editor's list to gain nothing.

The scale helpers §4.3 proposed are not here — see §8 for why.

---

## 8. Order of work

With `Harmony` shipped, the order that remained:

1. **`Chord`'s re-voice fix.** A bug before it is a feature: `Harmony` repeats
   a degree whenever `gravity` or `pedal` says so, and a repeated root
   currently produces no second chord at all.
2. **`Chord`'s `quality` enum.** Six typed intervals is the general case and a
   poor default; the eight useful stacks are all short.
3. **`Voicer`.** The empty row of §1, and the one that makes a fifth-walk
   sound like voices rather than like block chords.
4. **`Mirror`.** Cheap, and it changes the character of any patch it is
   dropped into.
5. **`Tonnetz`.** The chromatic complement, and it is voiced by `Voicer`
   rather than voicing itself.

The scale helpers of §4.3 are not on this list. `Voicer`, `Mirror` and
`Tonnetz` need pitch-class arithmetic and none of them need to know what a
mode's characteristic tone is; that helper belongs with whatever eventually
uses it, which would be a modal mode on `Harmony`.

### What changed in the building

- **The `Chord` fix was not a fix.** §6.5 called the missing re-strike on a
  repeated root a bug; `test_a_sequenced_root_walks_a_self_playing_chord_through_the_key`
  asserts it, in as many words — *the same root again changes nothing: the
  chord is already there*. The test is right and the design note was wrong to
  read a tested decision as an oversight, so the change is a parameter with
  the shipped behaviour as its default rather than a new default. Whether the
  hole in a repeated chord matters depends on what is downstream, which is
  exactly the shape of thing a switch is for.
- **`Voicer` does not take a mode with a bass constraint and a
  minimal-motion constraint at once**, because the two disagree and the
  disagreement is musical rather than a bug. Voice-lead C E G to F A C with
  the bass free and C stays put — total motion 3 semitones. Pin the bass to
  the chord's root and it is 15. Both are things people want, so `bass` is
  its own switch and the header says what it costs.
- **The ledger is keyed on the emitted note, not on the source note.** Every
  other modifier in the tree keys on the source, because it emits a function
  of one note and must release exactly what it sent. `Voicer` emits a
  function of the whole held *set*, and the question it has to answer on every
  chord change is "is this pitch already sounding" — which is a question about
  what was emitted. Keying it that way makes common-tone retention fall out:
  the notes in both voicings are neither released nor re-struck, because
  nothing asks them to be.
- **`Mirror` reflects the pitch class and then re-registers**, rather than
  reflecting the pitch. `2r + 7 - note` is the right map and the wrong octave
  — reflecting middle C about C's tonic-dominant axis gives a note below zero
  — so the reflection is taken modulo 12 and placed in the octave nearest the
  note that caused it. Which is also what makes `amount` usable: at 50% the
  reflected notes sit among the ones that passed through, instead of two
  octaves under them.
- **`Tonnetz` emits a root-position triad and leaves the voice leading to
  `Voicer`.** Parsimonious voice leading is the entire point of the P/L/R
  transforms, so a node that emitted them unvoiced looked wrong — until
  `Voicer` existed, at which point `Tonnetz -> Voicer` produces exactly the
  one-voice-moves motion by construction, and `Tonnetz` stays a generator
  like `Harmony` rather than a second voicer.

---

## 9. The patch this makes possible

```
Metronome (1 bar) ──advance──▶ Harmony ──root──▶ Chord ──▶ Voicer ──▶ Arpeggiator ──▶ MIDI out
                                                (note in unpatched)
```

No keyboard, no host, no sequence typed in. `Chord` free-runs, `Harmony`
walks it around the key and resolves to the tonic at the end of each phrase,
`Voicer` keeps the common tones so the chords move rather than jump, and the
whole thing is five nodes and one clock. Turn `gravity` up and it settles onto
a drone; switch the style to `modal` and it stops cadencing; put a `Mirror`
after the `Chord` and it plays its own shadow; replace `Harmony` with
`Tonnetz` and the same patch leaves the key entirely without any voice moving
more than a tone.

That is the test of whether this design is right: the interesting controls are
the ones a musician can name, every one of them stays in key by construction
unless it was asked not to, and none of them needed a note to be typed in.
