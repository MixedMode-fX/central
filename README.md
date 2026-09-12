# MixedMode Modular Central (MMMC)

An expandable Eurorack CV & MIDI processor on a Teensy 4.1.

- `GPIO_N` configurable gate jacks, each an input or an output.
- MIDI over two DIN ports (3.5 mm TRS), a class-compliant USB device with four
  cables, and a USB host port.
- No encoder, no switches, no display. The module is configured from a host:
  the serial console, the SysEx protocol, or the browser app.

Sizing constants are in `src/config.h`, pins in `src/hardware.h`. This file
names them rather than repeating their values.

## Build

```
make setup     # install PlatformIO into .venv/ and pre-fetch toolchains
make build     # firmware for the Teensy 4.1
make test      # native unit tests, no hardware
make size      # flash and RAM usage
make upload    # flash an attached Teensy
make app       # the WebAssembly build and the browser app (clang, lld, node)
```

`make` bootstraps PlatformIO on first use, so a fresh clone needs only
`python3`. The Teensy platform version in `platformio.ini` is pinned so the
core's MIDI Library and USBHost_t36 versions are reproducible.

Sources build with `-Wall -Wextra -Weffc++ -Wshadow -Werror`; framework
includes are passed as `-isystem` (`scripts/project_warnings.py`) so `-Werror`
judges our code only. Headers live next to their sources under `src/`;
`include/` and `lib/` are unused.

### Versions

`VERSION` at the root is the single source of truth. Builds stamp it with
`git describe` into `MMMC_VERSION`, `MMMC_GIT_REV` and `MMMC_BUILD`
(`src/version.h`). To release: update `VERSION`, commit, tag `v<version>` and
push the tag. The release workflow refuses a tag that disagrees with
`VERSION`, then publishes the `.hex`, `.elf` and `SHA256SUMS`.

### Tests

Hardware sits behind `IGpio` (`src/hal/igpio.h`) and `IMidiOut`
(`src/hal/imidi_out.h`). Teensy implementations are in `src/hal/teensy/`,
fakes in `test/fakes/`. Everything else is framework-free and builds on the
host: `pio test -e native`. Nothing under `src/algorithm/` includes
`Arduino.h`. CI builds firmware and runs the native tests on every push and
pull request.

## Signal bus model

Algorithms never bind to hardware. They read and write **buses**; the jacks
and MIDI endpoints are nodes too. A bus is a virtual patch cable.

| Domain | Carries | Count | Fan-in |
|---|---|---|---|
| Gate | a level (`bool`) | `N_GATE_BUS` | OR of all writers |
| Note | MIDI events | `N_NOTE_BUS` | arrival order; overflow counted |
| CV | `int16_t`, 12-bit full scale | `N_CV_BUS` | sum with saturation |

CV is the internal control bus: `CV_FULL = 4096`, unipolar `0..4095`, bipolar
`-2048..2047` (`src/bus/domain.h`). Twelve bits keeps eight summed writers
inside `int16_t` and matches the DAC a jack would use.

Buses are **double-buffered**: a bus is published once a pass, and until it is,
readers see the previous pass. The pool runs in the **graph's order** — every
writer of a bus before any reader of it — and each bus is published as soon as
its last writer has run (`src/node/schedule.h`). A signal therefore crosses the
whole graph in the pass that produced it, so two paths out of one event arrive
together however many nodes each of them crosses, and a reader still sees every
writer of a bus. A loop has no such order, so one of its edges keeps the delay:
feedback is a well-defined one pass rather than recursion. Every pass is
deterministic.

Every pass, `MixedModeMaster` runs:

1. input port nodes sample and write their buses, published together with every
   bus the pool does not write;
2. each pool node in turn: `process()`, then `tick()` if a clock tick fired and
   it subscribes, then the buses whose last writer it is;
3. while a transport stop is settling, `Node::transport_stopped()`;
4. the end of the pass, which publishes what those releases wrote;
5. output port nodes read their buses and drive the jacks and transports.

**Nodes.** Every algorithm is a `Node` (`src/node/node.h`), described by an
`AlgorithmDescriptor` in `src/node/registry.cpp`: id, name, summary, category,
inlets and outlets with names and domains, parameter count, state size and a
placement-new constructor. A `NodeConfig` selects an algorithm by id and gives
a bus index per port; `NO_BUS` leaves an optional inlet unconnected. Algorithm
ids are part of the preset format: append, never renumber.

**Allocation.** All algorithm code is resident. Instances live in a static pool
of `N_NODE` slots of `NODE_SLOT_SIZE` bytes (checked per class with
`static_assert`), placement-new'd on load and destroyed on unload. The jacks
and MIDI endpoints are reserved nodes owned by the master, outside the pool.
**No heap allocation after boot** — the native tests assert it.

**Parameters.** `NodeConfig` carries `N_PARAM` parameter bytes, sized by the
poly and drum sequencers' step grids. A zero byte means the parameter's
default (`src/node/param.h`).

**Handover.** Before unload the master calls `Node::silence()` on every pool
node, which releases everything it has sounding, then swaps and flushes the
MIDI outputs. Node state does not survive a swap.

## Master clock

One monotonic counter in **subticks**: `CLOCK_SUBTICK` per `MASTER_PPQN` tick.
The subdivision caps the fastest multiplier — `xN` is exact only when N divides
`CLOCK_SUBTICK`.

**No node's `tick()` runs in interrupt context.** The ISR increments; the main
loop reads the count through `MasterClock::consume()`. Subticks arriving
between passes collapse into one `tick()` with the newest count.

| Source | Driven by |
|---|---|
| Internal | interval timer, from BPM (`CLOCK_MIN_BPM`..`CLOCK_MAX_BPM`) |
| CV | rising edges on the sync jack, `cv_ppqn` per quarter |
| MIDI | MIDI clock bytes; start / stop / continue drive the transport |

For external sources the timer free-runs between edges at the last measured
period and each edge re-phases the count **forward only**, so a node never sees
time reverse. Multiplication from a gate source is refused: it would have to
extrapolate. Trigger width is wall-clock (`TRIGGER_WIDTH_US`), never ticks.

**Stop releases what the clock was playing.** A node that holds a note until
its next advance edge implements `Node::transport_stopped()`; the master holds
the stop against the pool until the gates that were in flight have drained,
then stops. A note the graph is only passing on belongs to whoever is holding
it, and a patch advanced from a jack is not the transport's: neither is
touched.

## Algorithms

| Domain | Algorithms |
|---|---|
| Logic | `NOT`, `AND`, `NAND`, `OR`, `NOR`, `XOR`, `XNOR` |
| Clock | `ClockDiv`, `Metronome` |
| Gate sequencers | `StepSequencer`, `EuclidianSequencer`, `RandomSequencer` |
| Note sequencers | `NoteSequencer`, `PolySequencer` |
| Drum sequencers | `DrumSeqGate`, `DrumSeqMidi` |
| MIDI modifiers | `Transpose`, `NotePriority`, `VelocityCurve`, `Chord`, `NoteQuantise`, `Probability`, `Arpeggiator`, `NoteDelay` |
| Harmony | `Harmony`, `Voicer`, `Mirror`, `Tonnetz`, `Key` |
| Routing | `NoteFilter`, `Channel` |
| Conversion | `Sustain`, `GateToNote`, `MidiToCV`, `CvToNote`, `CvToGate` |
| Utility | `GateHold` |
| Modulators | `LFO`, `SampleHold`, `Slew`, `Turing` |
| Rhythm | `Automaton` |

Every algorithm reports its own name, summary, category, port names, parameter
ranges, defaults and enum options over the protocol, so a host never hardcodes
a list, and `params <node>` on the console prints the same thing. What follows
is only what a parameter list cannot say.

- **Division is a node.** `ClockDiv` writes a gate bus and sequencers take an
  edge-triggered advance inlet, so one divider can drive several (locking them
  by construction), a divider can divide a divider, and any gate can advance a
  sequencer. `Metronome` is the same clock in note values with a straight,
  dotted or triplet feel; every one of its rates is a whole number of subticks,
  asserted against `config.h` at compile time.
- **All three sequencer families share `StepEngine`**
  (`src/algorithm/sequencer/step_engine.h`), so a gate and a note sequencer at
  the same length and direction visit steps identically.
- **Note sequencers store a scale degree, never an absolute note.** Pitch is
  `root + degree_to_semitone(degree, scale)`, and both come from the key
  (`src/midi/global_key.h`): moving the key transposes the pattern and changing
  its scale re-reads it as an interval shape. All a sequencer says for itself
  is the register it plays in. The root comes from the root inlet when patched,
  last note-on wins. Length is counted in advance edges and is exact; the
  sub-step `gate` percentage extrapolates from the last two advances and is
  therefore wrong after a tempo change. Ratcheting and real-time record need
  the same estimate and are deliberately not built.
- **A drum pattern is one node**, a `StepEngine` per lane, because a user edits
  the grid as one object. Advance and reset are shared and **each lane has its
  own length**, which is polyrhythm for free. `DrumSeqGate` has a gate per
  lane (accent is a second lane); `DrumSeqMidi` has one note outlet with a note
  number and channel per lane and a velocity per cell.
- **`GateHold` turns a trigger into a gate** — `latch`, `toggle`, `extend`
  (minimum length), `limit` (maximum length). **Reset wins** over `set` in
  every mode, and a mode change is not a reset. Both inlets are optional: the
  `gate` parameter *is* the output level, so with nothing patched a latch is a
  switch that a CC or a modulation route can also throw.
- **Modulators write a CV bus** and know nothing about what they modulate; the
  modulation matrix decides what the signal reaches. A synced `Lfo` is anchored
  on subtick zero rather than on construction, and changing its rate re-derives
  the period without moving the phase. `Slew` rates are **per full scale**, so
  two different step sizes glide at the same speed.
- **`CvToNote` and `CvToGate` are the doors from the CV bus into the musical
  domains**, and `MidiToCV` is the inverse, which makes the CV bus a round
  trip. `CvToNote` is not `NoteQuantise`: it takes a *level*, with no events at
  all, and decides both what to play and when. `degree` divides the range into
  the scale's notes evenly; `snap` divides it into semitones and snaps, which
  preserves the shape of a signal that was already a melody. `CvToGate`'s
  hysteresis is not decoration — a signal resting on the threshold would
  chatter into a stuck note.
- **`MidiToCV` is one voice, because a CV pair is one voice.** Which note wins
  is a `NotePriorityRule` (`src/midi/held_notes.h`), asked here rather than
  patched in front because the answer also decides when the gate falls, when
  the trigger fires and which velocity the voice takes. Pitch is carried with
  eight sub-bits so bend is not rounded away, and bend is part of the pitch
  because a DAC has one input per jack. The gate follows the key; the trigger
  is a fixed-width pulse on every attack. Sustain is deliberately not handled
  here — it is an operation on the note stream, so it belongs upstream.
- **`Turing` is the random source with memory.** `length` bits in a ring, the
  top bit fed back inverted with probability `chaos` — 0 is a fixed loop, 50 is
  noise, 100 is a loop of twice the length, and between 0 and 50 the loop
  mutates a step at a time. Its pulse and its CV come from **one register**, so
  a rhythm and a melody driven from it change on the same bar. `seed` decides
  what reset means: at zero it shreds, otherwise it returns to that pattern.
- **`Harmony` only emits a root.** `Chord`'s qualities are scale steps, so the
  quality of each degree is already correct. The weight of every move is
  computed from the scale (`src/midi/root_motion.h`) rather than tabulated, out
  of facts that hold in any key: `fifths` (which way round the circle, measured
  in **semitones**, so it finds a pentatonic key's fifths and declines to
  invent the ones it has not got), `smooth` (interval distance versus shared
  tones), `leading`, and `spread`, which flattens or sharpens the weights and
  never zeroes one — so an unlikely move is always available and always in key.
  `phrase` and `cadence` make it periodic, `loop` fixes the next phrase,
  `gravity` biases the tonic, `drift` redraws one chord of a loop and keeps it.
  The walk runs over the first seven degrees, or all of them in a smaller
  scale.
- **`Automaton`'s lanes are neighbours.** A Wolfram elementary rule,
  `next[i] = (rule >> ((left << 2) | (self << 1) | right)) & 1`, so a figure on
  one lane moves to the next generation's neighbour and a lane with no jack
  still feeds the ones that have. `revive` reloads the seed on a row that can
  never change again; a row that merely blinks is a rhythm and is left alone.
- **`NoteDelay`'s `spread` is why it exists**: each successive gap is a
  percentage longer (or shorter) than the last, which takes the echoes off the
  grid — the one thing a module where every edge descends from one divider
  could not otherwise do. Repeats are transposed by `interval` **scale steps**.
  Each pending echo carries the pitch it will be released with, so the key and
  the interval can move underneath it, and `dry` copies are owned too.
- **`Voicer`'s ledger is keyed on the note it emitted**, not the note that
  caused it, because it emits a function of the whole held chord rather than of
  one note. Common-tone retention is then the *absence* of code: a shared note
  is neither released nor re-emitted. `bass` pins the lowest voice to the
  chord's bass and disagrees with minimal motion, which is why it is a switch.
- **`Mirror` reflects the pitch class and then re-registers** into the octave
  nearest the source note; reflecting the pitch itself gives numbers that are
  not notes. `snap` puts the result back in the scale and is off by default,
  because a reflection that stayed in the key would be a transposition.
- **`Tonnetz` is the other walk.** P, L and R each move one voice by a semitone
  or a tone; alternating two of them traces a cycle — `LR` fifths, `PL` major
  thirds, `PR` minor thirds. `deviation` is the chance of leaving the cycle,
  `diatonic` refuses triads the key does not hold. Its `root` inlet plays it
  the way `Chord`'s does — a note-on starts the walk again on that note,
  register and all, and does not move the key — and the
  triad it starts on is whichever one the key holds there, so the quality is
  not a setting. It emits root position and leaves the voice leading to
  `Voicer`.
- **A note bus is already a splitter, so `NoteFilter` is the split.** A bus
  fans out to every reader and in from every writer, so three filters on one
  bus with different ranges are a three-zone keyboard split, and pointing them
  at one bus rejoins it — which is why the node has one outlet rather than
  several. Every test is taken on the note-on and the note-off is looked up in
  the ledger, so a window that closes under a held note still releases it, and
  a note-off whose note-on was dropped is dropped too. `not channel` inverts
  the channel test alone: the other clauses each have their own way round
  already, and inverting all of them would take away "the notes, but not the
  ones on channel 10".
- **`Channel`'s `count` is why it is not a relabelling.** A span of one is a
  fixed channel; wider, the parameters name a span of that many channels
  wrapping at 16 and each note-on is allocated one of them, which makes four
  mono synths a polysynth played from one keyboard. Allocation skips a channel
  this node still has sounding, because a stolen voice beside an idle one is
  nothing but round-robin showing through. The note-off leaves on the channel
  its note-on did, so both parameters can move under a held chord.
- **Logic gates fold over every input in the mask** from the gate's identity
  element, so **XOR over more than two inputs is parity**. Inputs are
  normalised in the HAL (`GATE_INPUT_ACTIVE_LOW`), so an unpatched input reads
  0 and cannot force an OR high. `NOT` and `Sustain` take exactly one port and
  are invalid otherwise. Logic is not clocked by the master clock.

## MIDI

All four transports feed the same graph. Every parser drains into one queue
tagged with its transport; the pass dispatches. A full queue drops the newest
message and counts it. Realtime messages are transport-level and reach the
master clock, not a bus.

**Thru is a patch, not a default.** Library soft-thru is off; a `MidiInPort`
and a `MidiOutPort` on a shared note bus is thru.

**Modifiers** compose over one held-note model (`src/midi/held_notes.h`). The
rule that governs all of them: **a modifier owns the note-off for every note-on
it emitted and releases it with the transformation it originally applied**,
never the current parameter value. A modifier that cannot record an emission
does not emit.

`Arpeggiator`'s hold is a parameter and an optional gate inlet. While hold is
on, the next note-on after every key has been released replaces the figure
rather than adding to it; taking hold off keeps what is still physically held.

### The key

One scale, one root and one register for the whole module, in the patch's
`GlobalSettings` (`src/midi/global_key.h`). A scale is a 12-bit mask, one bit
per semitone (`src/midi/scale.h`).

**There is no second copy of it.** No algorithm carries a `scale`, a `key` or a
`root` parameter: three ways for a node to leave the key it was in turned the
one decision a musician makes into twenty-four parameters that had to agree,
and nobody wants a patch in two keys at once. What a node still chooses is the
register, because a bass line and a lead are the same key two octaves apart —
one `octave` parameter, where 0 (the default) is the key's own register and
1..10 names one outright. So one setting moves the whole patch and a part that
has been placed keeps its place. **A patched root inlet outranks all of it.**

Set the key from the console (`key <scale> <root> <oct>`), over
`SYSEX_SET_GLOBALS`, on the app's key page — or from inside the patch:

- **`CC_TARGET_KEY`** makes the key a target for a controller, an NRPN address
  and a modulation route, through the same applier every other control-plane
  write goes through, with no node in the patch at all.
- **`Key`** is a node whose note inlet moves the key's root: a Harmony walking
  the degrees of one key is a progression, and a sequencer moving the key under
  it every eight bars is a piece with sections. It holds no key of its own and
  writes only the key that is *playing*, so a preset saved mid-phrase records
  the key the patch was written in — the rule a modulated parameter follows.
  One per patch, because the key has one value; it runs before every node that
  plays in the key, so a change is heard by the notes of the same pass.

The module is chromatic on C until a key is set.

`Chord` is three questions and one parameter for each: `quality` names a stack
of **scale steps**, so one setting is a diatonic triad — or seventh, or ninth —
on every degree of the key; `inversion` says which voice is in the bass;
`voicing` says how far apart they sit. A chromatic key has no degrees to colour
a chord with, so there each quality plays its own shape in semitones instead.
With nothing patched to its note inlet `Chord` **plays itself**: the tonic
chord of its key, held, which makes a chord + metronome + arpeggiator a
complete patch with no input. A held chord is re-voiced whenever
what it should play changes, releasing from the ledger first. A repeated root
does not re-strike unless `retrigger` is set.

## Control and feedback

There is no human input on the module, so the console and the SysEx protocol
are the only way to configure it. **Neither is a graph node**, neither is
reachable from a bus, and a patch cannot disable either — a bad patch must not
be able to lock you out.

### Status LEDs

`GREEN_LED` and `RED_LED` are the whole feedback surface. Both are PWM, so
brightness is part of the vocabulary. Nothing in the LED path blocks.

| What you see | What it means |
|---|---|
| Green, bright flash on the beat | the clock is running; the flash rate is the tempo |
| Green, slow dim pulse | alive, no clock |
| Red, solid | no valid patch; running the built-in default |
| Red, brief flash | something dropped or refused; `errors` has the counters |
| Both, alternating | boot, and the answer to a device inquiry |

### Boot

Slot 0 is loaded if it validates. Otherwise the **built-in default patch** runs
— MIDI thru across every musical transport, a `Metronome` at a quarter note on
jack 1, a sustain pedal input on jack 8 — so a freshly flashed module is
observably alive. A corrupt stored patch also lights the red LED; an empty
store does not. Restoring defaults is host-side (`defaults`, or SysEx).

### Patch storage

A patch is the node list, the bus per port, and the global settings. It is
stored in flash-emulated EEPROM (`EEPROM_BYTES`) as `PATCH_SLOTS` independent
images, each with its own magic, format version and CRC. Slot 0 is current; the
rest are Program Change presets. A slot failing CRC cannot affect the others.

The stored image trims each node's parameter block at its last non-zero byte,
and the same encoding goes on the wire, so a patch round-trips through the
store and over SysEx byte for byte.

Nothing writes flash on a parameter change: an edit marks the patch dirty and
the autosave writes slot 0 once, two seconds later.

### Serial console

USB serial and `SERIAL_UART` both carry the same text console.

| Command | What it does |
|---|---|
| `info` | build, node count, store state |
| `clock [bpm] [source]` | show or set tempo and clock source |
| `key [scale] [root] [oct]` | show or set the key |
| `patch` | the running patch: jacks, ports, nodes, connections |
| `buses` | live bus state and overflow counters |
| `errors` | every counter behind the red LED |
| `algos` | every algorithm, with port counts |
| `params <node>` | one node's parameters, ranges, defaults, enum names |
| `get` / `set <node> <param> [value]` | read or write a parameter |
| `slots`, `save` / `load` / `erase <slot>` | presets |
| `defaults` | back to the built-in patch |
| `maps` / `map` / `unmap` / `learn` | controller bindings |
| `mods` / `mod` / `unmod` | modulation routes |

### SysEx

Everything the console can do, plus enumeration and bulk transfer. **The wire
format is the patch format** — there is no second representation to drift.

Framing is `F0 7D <device> <command> <version> … F7`. `0x7D` is the
non-commercial manufacturer ID and **must never ship in a product**. The
version byte is in every message, so a mismatch is refused per message rather
than mid-transfer. Binary payloads are 7-in-8 packed.

- **Discovery.** The module answers the Universal identity request
  (`F0 7E <dev> 06 01 F7`) and blinks both LEDs, then reports its capabilities
  and enumerates every algorithm and parameter descriptor off the compiled
  table — names, summaries, categories, port names, ranges, defaults, enums.
- **14-bit values.** A parameter byte is eight bits and a SysEx data byte is
  seven, so `SYSEX_SET_PARAM` carries the eighth bit in an optional argument
  and `SYSEX_PARAM_VALUE` and `SYSEX_PARAM_DESC` answer with 14-bit values.
- **Two tiers of write.** A bulk transfer is chunked with a sequence number and
  a checksum per chunk and accumulates into a staging buffer; the live graph is
  untouched until the whole image passes magic, version, CRC and validation. An
  incremental edit is one message changing one field.
- **Program Change recall** is off by default, with a configurable channel and
  port. A recall can be immediate or quantised to the next beat or bar;
  immediate with the clock stopped. The module announces a recall to the host.

What survives a change:

| Change | Reconstructed |
|---|---|
| A parameter | nothing |
| A connection | only that node — it gets its handover and starts fresh |
| A port | nothing; port nodes are configured, not constructed |
| A whole patch | everything |

### Controller mapping (MIDI CC)

`N_CC_MAP` bindings live in the patch, apply at the MIDI input layer before the
graph runs, and write through the same validated entry point the console and
SysEx use. Deliberately **not** a node: a parameter has no domain, no fan-in
rule and no per-pass value.

A target has a kind: `node` (index plus parameter), `clock` (tempo, source, CV
PPQN), `transport` (start, stop, continue, tap tempo) and `key` (root, scale,
register). `port` is reserved. The last is a kind rather than somebody's
parameter because the key is one setting for the whole patch and no node
carries a copy of it.

- **14-bit**, as CC *n* MSB and CC *n*+32 LSB, because tempo does not fit in
  seven bits. A lone MSB is applied rather than stalling.
- **Takeover**: `jump` (default), `pickup`, `scale` (anchor on first move and
  map the travel either side of it).
- **Relative encoders** in all three encodings (two's complement, signed bit,
  offset-64). A relative mapping sidesteps takeover entirely.
- **Pass-through** is off by default.
- **Learn** binds the next CC seen, times out, and ignores the control cable.
- **Rate limiting at the pass boundary**: only the newest value per mapping
  survives to the next pass.

### Modulation

`N_MOD_ROUTE` routes in the `Patch` (`src/control/mod_matrix.h`), built like
`CcMapper` and ending at the same `PatchManager::set_param`. Runs between
passes, after `CcMapper::apply` and before `MixedModeMaster::pass`, reading the
front buffer — so a route sees a published value and order does not matter.

- **Absolute** sweeps the target across the route's range; **offset** keeps the
  parameter's own setting as a centre. Each offset lane remembers what it
  wrote, so a target that has moved elsewhere re-takes the centre.
- Each route carries a depth, a sub-range in the target's units, and
  `bipolar` and `invert` flags. Polarity belongs to the route, not the bus.
- **Two routes may not share a target** — the validator refuses it; the CV bus
  is where two modulators mix. A route cannot reach a `transport` target.

One write per route per pass. Routes travel in the patch image, over SysEx as
`SET_MOD_ROUTE` / `GET_MOD_ROUTE`, and through the console as `mods`.

### NRPN

The rule: **if it changes the graph's shape it is SysEx; if it changes a value
inside a node it is CC or NRPN.**

| Tier | Carries | Use |
|---|---|---|
| CC | one 7-bit scalar, or 14-bit as a pair | performance |
| NRPN | 14-bit address + 14-bit value | every parameter, addressed |
| SysEx | arbitrary length | structure, patterns, bulk, enumeration |

```
0x0000 .. 0x39BF   a node's parameter: node = address / N_PARAM,
                                       param = address % N_PARAM
0x39C0 .. 0x39CF   the master clock (tempo, source, CV PPQN)
0x39D0 .. 0x39DF   the transport (start, stop, continue, tap)
0x39E0 .. 0x39EF   the key (root, scale, register)
0x39F0 .. 0x3FFF   reserved
```

The bases move when `N_NODE` moves, and the protocol version with them. This
address space, not memory, is the ceiling on the pool. The capability message
reports it so an editor does not hardcode it.

NRPN is **off by default**, enabled per port and channel, because CC 99, 98, 6
and 38 look like ordinary CCs to everything upstream. A partial sequence writes
nothing and times out. Data Increment / Decrement (CC 96/97) read the current
value rather than tracking it.

### Step-record

A note sequencer stores degrees and a keyboard sends pitches, so entry is a
conversion. A `record` note inlet and a `record enable` gate inlet: a note-on
writes the step under the record cursor and advances it; reset returns both
cursors to the first step. A note outside the scale snaps to the nearest tone
in it and the snap is counted. `rest key` and `tie key` (default MIDI notes 0
and 1, configurable) enter rests and ties.

## The module in a browser

The same framework-free core compiles unchanged to WebAssembly with the browser
as the hardware. `emulator/` is the web implementation of `IGpio` and
`IMidiOut`; `app/` is the app that drives it — the patch editor and the
emulator are one program, because the module in the page is both the thing
being edited and the thing running.

```sh
make app                         # clang, lld and node; no PlatformIO
open emulator/dist/index.html    # a single self-contained file
```

The build from `main` is at <https://mixedmode-fx.github.io/central/>, which is
also what satisfies Web MIDI's secure-context requirement. See
`emulator/README.md` for the seam and its limits, `app/README.md` for the app.

## Not built

- **The KeyMech header.** `SERIAL_KEYMECH` with boot and reset lines is
  declared in `hardware.h` and nothing says what it connects to.
- **CV at the jacks.** The CV domain has writers and readers but no pins. A
  `CvOutPort` on the DAC and a `CvInPort` on the ADC would need no change to
  the matrix, the patch format or the app. Calibration has a home reserved in
  `GlobalSettings`.
- **Real-time record** into a note sequencer, and **ratcheting**: both need the
  same step-duration estimate the sub-step gate uses, which should prove itself
  on hardware first.
- **Launchpad DAW mode** over the USB host port — the only realistic hands-on
  control surface, and it needs no new pins.
- **microSD presets.** The socket is on dedicated SDIO pins and needs nothing
  from `hardware.h`, so it can be added without touching the hardware surface.
