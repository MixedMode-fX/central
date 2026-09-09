# Firmware architecture review

A review of the firmware under `src/` (about 11 300 lines) and the emulator seam
under `emulator/src/`, read in full, with the native test suite as the baseline
(288 tests, all passing at `63ccd54`). Where a finding is a claim about
behaviour rather than about structure, it was checked by writing a throwaway
Unity test against the real firmware objects and watching it fail; those probes
are described inline so they can be re-run.

The short version: the core model is sound and unusually well argued, and the
place where it is weakest is not the graph but the control plane around it,
where three concerns (time, error reporting, and the target space for
parameter writes) have no single owner. The most valuable fixes are small.

## Contents

1. What is good, and should not be touched
2. Defects found while reviewing (verified)
3. Abstraction opportunities
4. Simplification opportunities
5. Scalability
6. A suggested order of work

---

## 1. What is good

These are the decisions the rest of the review assumes stay:

- **Everything is a `Node` reading and writing bus indices.** The hardware
  jacks and MIDI endpoints being nodes too, outside the pool, is what makes the
  graph order-independent and a patch unable to delete its own MIDI output.
- **Double-buffered buses with per-domain fan-in rules.** One pass of latency
  per stage, feedback as a one-pass delay, deterministic passes. This is the
  right shape and the tests lean on it well.
- **The note-off ledger (`SoundingNotes`).** "A modifier releases what it
  sent, never what it would send now" is enforced by construction, and every
  emitting node uses it. This is the single most important invariant in a MIDI
  processor and it is in one place.
- **Descriptors as the source of truth.** Ranges, names, defaults and port
  domains are compiled tables read by the validator, the console, the SysEx
  enumeration and the app. Nothing is hand-copied into the editor.
- **No heap after boot, one uniform pool, `static_assert`-checked slot size.**
- **The two seams (`IGpio`, `IMidiOut`) plus `IEeprom`, `ILeds`,
  `IConsoleIo`, `ISysexIn`** are narrow enough that the same object code runs
  on the Teensy, in the tests and in a browser. That is rare and worth
  protecting.
- **The wire format is the storage format.** One codec, one CRC, one
  validator.

---

## 2. Defects found while reviewing

Listed first because they change what "optimisation" means: several of these
are hidden by the test rig and the emulator and will only show on hardware.

**Status.** Steps 1 to 4 of the plan in section 6 are done on this branch,
each as its own commit with tests:

| Finding | Fix |
|---|---|
| 2.1 SysEx has no clock | `ISysexIn::deliver_sysex()` takes `now_us`; the Teensy transport passes the pass's `micros()`, the emulator the page's time. Four tests run the handler at realistic uptimes. |
| 2.2 Bursts overflow the note bus | The drain moved to `control/midi_dispatch.cpp`, peeks before it pops and stops in front of a bus with no room. `NOTE_QUEUE_DEPTH` is 32. Three tests in `test_nrpn`. |
| 2.3 Two owners for clock settings | The console edits through `PatchManager::set_globals`. One test in `test_storage`. |
| 2.4 Silent reply truncation | `put()` marks an overflow and `send_reply()` sends a NAK instead. |
| 2.5 `Patch` on the stack | `PatchStore` owns a scratch and a `probe()`; every caller loads into a buffer it already owns. No `Patch` is declared on the stack in `src/`. One test. |

Everything from 2.6 onward is still open.

### 2.1 The SysEx path has no clock, so every timed behaviour it triggers is wrong on hardware

`ISysexIn::deliver_sysex()` (`src/hal/isysex_in.h:16`) carries no timestamp.
`SysexHandler::deliver_sysex()` therefore hands `now_us = 0` to every command
(`src/protocol/sysex_handler.cpp:104` and `:123`). Everything downstream that
compares `now_us` against a stored time then measures from boot instead of from
the event:

| Path | Effect on a module that has been up more than a few seconds |
|---|---|
| `receive_chunk` sets `transfer_started_us = 0`; `service()` (`sysex_handler.cpp:748`) aborts a transfer older than 10 s | **A chunked patch transfer fails after 10 s of uptime.** The app awaits an ACK per chunk (`app/src/device.js:279`), so `service()` runs between chunks on hardware and abandons the transfer before chunk 2 arrives. |
| `SYSEX_CC_LEARN` arms with `learn_armed_us = 0`; `CcMapper::observe` cancels a learn older than 20 s (`cc_mapper.cpp:94`) | **Learn over SysEx never binds after 20 s of uptime.** Console learn works because the console passes real time. |
| every SysEx edit calls `store.mark_dirty(0)`; `PatchStore::service` saves once `now - 0 >= 2 s` (`patch_store.cpp:105`) | **The autosave debounce is defeated for SysEx edits**: flash is written on the very next loop iteration, and again after every subsequent edit, instead of once two seconds after the burst. An editor slider sweep becomes a flash write per message. |
| `leds.identify(0)` on HELLO and on the universal identity request | The identify blink never shows after the first second of uptime. |
| `arm_swap(0)` for `SLOT_LOAD` and `RESTORE_DEFAULTS` | Harmless today (only compared with the subtick count), but the same shape. |

Why the suite and the emulator miss it: `test_sysex` calls `service()` with
small absolute times, and in the app all chunks are delivered on microtasks
before the next animation frame calls `emu_control_service()`
(`app/src/module.js:129`, `:173`). Both hide a bug that a real USB round trip
exposes.

Verified with a probe: a rig at 11 s uptime sending a two-chunk patch with one
`service()` call between chunks is refused with `SYSEX_ERR_NO_TRANSFER`; a learn
armed over SysEx at 25 s uptime does not consume the next CC; a `SET_PARAM`
edit is written to the fake EEPROM on the following `service()` call.

Fix (small, and an abstraction fix rather than four patches): give the control
plane one time source. Either add `uint32_t now_us` to `deliver_sysex()` and
`handle_command()` and have the transport pass `micros()`, or queue complete
SysEx messages the way MIDI events are queued and dispatch them from
`loop()` with the pass's `now`. The second is cleaner: it also takes the whole
patch apply out of `mm_midi_read()` and makes the transport side "enqueue and
nothing else" for SysEx as it already is for channel messages.

### 2.2 A MIDI burst larger than the note bus is silently truncated, and note-offs are what bursts are made of

`deliver_midi()` writes straight into the back buffer of a note bus, which
holds `NOTE_QUEUE_DEPTH = 16` events per pass (`config.h:23`,
`bus_manager.cpp:50`). The loop drains up to 64 queued events per iteration
(`main.cpp:98`), and the transports each deliver up to 16 per iteration
(`teensy_midi.cpp:57`), so one pass can easily be offered 20 to 80 events.
Anything past 16 on one bus is counted and dropped.

The common burst is a sustain-pedal release or an "all notes off" from a DAW:
10 to 30 note-offs in one USB packet. Dropping four of them hangs four notes on
the downstream synth, which is exactly the failure the whole ledger design
exists to prevent.

Verified with a probe: 20 note-offs delivered to one bus before a pass produce
16 messages at the MIDI output.

Fix: make the drain back-pressure aware. Peek the queue, stop draining this
pass when the target bus refuses the write, and leave the rest for the next
pass (the input queue holds 64, and a pass is sub-millisecond). Raising
`NOTE_QUEUE_DEPTH` alone only moves the cliff; the drain-until-full rule is
what removes it. This is a five-line change in `main.cpp` plus a
`note_room(bus)` accessor on `BusManager`.

### 2.3 Clock settings have two owners

`Console::cmd_clock` writes tempo and source directly to `MasterClock`
(`console.cpp:181`, `:187`). `CcMapper::write_control` and the SysEx
`SET_GLOBALS` path write through `PatchManager::set_globals`, which pushes all
three clock fields from `live_globals` (`patch_manager.cpp:11`). After
`clock 140 1` on the console, the first CC tempo move pushes the *stored*
source back, silently undoing the console change, and nothing the console set
is ever saved.

Fix: `GlobalSettings` is the source of truth; the console goes through
`set_globals` like everyone else, and `MasterClock` setters become private to
that path (or `PatchManager` becomes the only caller).

### 2.4 `SysexHandler::put()` truncates replies silently

`put()` drops any byte past `SYSEX_TX_MAX - 1` (`sysex_handler.h:100`) and
`send_reply()` still appends `F7` and sends. A reply that did not fit goes out
looking well-formed with a short tail. `test_params` budgets today's names
against 320 bytes, so it does not happen now, but the failure mode is a
corrupt enumeration rather than an error, and it will happen the day an
algorithm gets a fifth inlet with a long name and a long summary. Track an
overflow flag in `put()` and turn it into a NAK.

### 2.5 The "no big things on the stack" rule is stated and then broken in the same file

`patch_store.cpp:9` keeps the encode buffer static because "a slot is a
kilobyte and the Teensy's main stack is not the place for it". Fifty lines
later `PatchStore::load()` puts a `Patch` on the stack (`patch_store.cpp:59`),
and `occupied()` puts another one under it (`:70`). `sizeof(Patch)` is
**11 690 bytes**, not a kilobyte. The same happens in `PatchManager::boot`,
`recall_slot`, `SysexHandler::receive_chunk`, `SLOT_LOAD` and
`program_change`. `reply_slots` decodes every slot twice (`occupied()` then
`used()`), each time through 23 KB of stack.

It works today because the Teensy 4.1 has a large stack and the wasm build
reserves 64 KB, but it is the kind of thing that fails first when a Patch
grows (section 5.1). Fix: one static scratch `Patch` inside `PatchStore`, and a
`probe(slot)` that checks magic, length and CRC without decoding (section 4.3).

### 2.6 Smaller things

- **`SLOT_LOAD` uses stale swap timing.** `SysexHandler::timing` is only
  refreshed from `pc_quantise` on `SET_GLOBALS` and on a Program Change
  (`sysex_handler.cpp:255`, `:791`). After a boot from slot 0, a SysEx slot load
  is quantised according to whatever `timing` last was, not the stored
  setting. The root cause is that `timing` duplicates a field of
  `GlobalSettings` (section 4.2).
- **A main-loop write races the sync ISR.** `MasterClock::set_source()`
  recomputes `edge_index` from `subticks` on the main thread while
  `external_edge()` updates both from the interrupt (`master_clock.cpp:45`,
  `:112`). Same for `start()` zeroing `subticks` under the timer ISR. The
  outcome is one mis-phased edge, so it is minor, but the HAL has no
  critical-section seam at all; the clock is the one place that needs one.
- **`NoteSequencer` (mono) is the same 544 bytes as `PolySequencer`.**
  `steps[STEP_BYTES]` is sized for `MAX_VOICES` regardless of `n_voices`
  (`note_sequencer.h:233`). In a uniform pool it costs nothing, but it is the
  reason the slot cannot shrink and it makes the constructor copy 320 bytes it
  will never read.
- **Dead `EdgeIn`.** `NoteSequencerBase::rec_enable_in` is constructed and
  ticked "to keep the edge detector in step" (`note_sequencer.cpp:380`) but
  its edge is never used; `rec_enable_bus` does the work. Delete one.
- **`param_effective()`, `gpio_map_write()`, `gpio_map_read()`** have no
  callers. `set_swap_timing()` is used only by a test.
- **Include guards** `__GATES_H_`, `__SUSTAIN_H_`, `__VERSION_H_` are
  reserved identifiers; the rest of the tree uses `MMMC_*`.
- **The README has drifted from the code** in places a new contributor would
  trip on: `is_valid()` and port masks (`README.md:157`) no longer exist;
  `ASTABLE` and `LATCH` are listed but not implemented; the class diagram shows
  `MidiModifier` and `HardwarePort` base classes that are not in the source;
  "an algorithm claims its outputs in `setup()`" describes the pre-bus model.

---

## 3. Abstraction opportunities

### 3.1 The zero-means-default rule is implemented three times per parameter, per algorithm

The rule is stated once in `param.h:29` and the descriptor carries `def`, but
no node uses it. Every algorithm hand-rolls it in three places that must agree:

```
constructor:  channel(config.params[0] ? config.params[0] : 1)
set_param:    case 0: channel = value ? value : 1; return true;
get_param:    case 0: return channel;
```

Across the 25 algorithms that is roughly 700 lines of `switch` whose only job
is to move a byte between a `NodeConfig` and a member, and they already
disagree in small ways: `Arpeggiator`'s constructor clamps `octaves` above
`MAX_OCTAVES` while `set_param` rejects it; `NoteSequencerBase` wraps the
channel with `((v-1)%16)+1` at construction and rejects `>16` at runtime;
`DrumSeqMidi` masks a note with `& 0x7F` at construction and rejects `>127`
later. None of it matters because the validator already enforces the ranges
before either path runs, which means the constructor clamps are dead code that
merely looks like a second policy.

The symptom at the call site is `PatchManager::set_param` having to call
`get_param` after `set_param` to find out "what the node actually took"
(`patch_manager.cpp:155`), and `SYSEX_SET_PATTERN` going through the full
`set_node_param` lookup once per byte.

Proposal: let the descriptor drive storage. A `Node` base (or a `ParamBlock`
member) owns `uint8_t stored[n_params]` copied from the config, with
`set_param` and `get_param` implemented once against the `ParamGroup` table.
Each algorithm reads effective values through one inline accessor
(`p(P_CHANNEL)` returning `param_effective(desc, stored[i])`) and overrides a
single hook, `on_param_changed(index)`, for the few parameters that cache a
derivation (Euclid's pattern, ClockDiv's period, trigger width). The
constructor's job becomes "copy the block and derive", the three copies of
every default collapse to the descriptor's `def`, and the mirror-after-write in
`PatchManager` goes away because the stored byte *is* the value.

Cost: the sequencers already keep their step bytes this way
(`steps[]`, `velocity[][]`), so for them it is a rename. For the small nodes it
adds a few bytes of state per parameter, well inside the slot.

### 3.2 The base `Node` should own its bus indices

Every node copies the subset of `in_bus`/`out_bus` it cares about into named
members at construction. That is why a connection edit has to be a full
reconstruction (`master.h:88`, "making it anything else would mean a re-bind
seam on all 25 algorithms"). If `Node` held `uint8_t in[MAX_IN]` and
`out[MAX_OUT]` (13 bytes), a rebind would be a byte write plus a `silence()`,
a running sequencer would keep its position when a cable is dragged, and the
per-node `EdgeIn advance_in(config.in_bus[0])` boilerplate would read from the
base. This pairs naturally with 3.1: `NodeConfig` minus the algorithm id *is*
the node's persistent state, so the base class can hold exactly that.

### 3.3 The note-modifier skeleton is written four times, and the README already names the class

`Transpose`, `Chord`, `NoteQuantise` and `Probability` have identical `process()`
loops: note-off releases from the ledger, note-on transforms and emits,
everything else passes through, `silence()` releases all. The README's diagram
shows a `MidiModifier` base with `HeldNotes` and `SoundingNotes`; the code
does not have it. A `NoteModifier : Node` with `virtual void on_note_on(bus,
const MidiEvent&)` and the ledger as a member would remove about 120 lines and
make the ownership rule structural rather than a convention each new modifier
has to re-learn. `NotePriority` and `Arpeggiator` (which also track held notes)
fit the same base with `HeldNotes` added.

### 3.4 Edge detection exists as a component and is bypassed

`EdgeIn` (`step_engine.h:122`) is used by the sequencers; `Arpeggiator`
(`last_advance`, `last_reset`), `ClockDiv` (`last_gate`), `GateToNote`
(`last`) and `Sustain` (`last_raw`) each hand-roll the same two lines. Use
`EdgeIn` everywhere, and give it a `level()` for the nodes that want both.

### 3.5 "Root from a note bus, last note-on wins"

Identical loops in `NoteQuantise::process` (`note_quantise.cpp:55`) and
`NoteSequencerBase::process` (`note_sequencer.cpp:363`). A six-line
`latest_note_on(const BusManager&, uint8_t bus, uint8_t& note)` helper next to
`is_note_on` removes the duplication and makes the rule greppable.

### 3.6 Trigger width in milliseconds

`value ? value * 1000 : TRIGGER_WIDTH_US` appears in `GateSequencer`,
`ClockDiv` and `DrumSeqGate` (twice each, constructor and `set_param`). Give
`TriggerPulse` a `set_width_ms(uint8_t)` that applies the zero-means-default
rule once.

### 3.7 The control-target space is a real abstraction that lives in the wrong class

`CcMapper::target_range / write_control / read_control` are how NRPN, the
SysEx `GET_CONTROL` command and the console resolve a `(kind, index, param)`
triple to a node parameter, a clock field or a transport action. `NrpnDecoder`
depends on `CcMapper` only for this. It is a `ControlTarget` facade, not a CC
concern. Extract it (a small class or namespace over `PatchManager` and
`MixedModeMaster`) and both `CcMapper` and `NrpnDecoder` become pure
transports for it, which is what their own header comments say they are.

### 3.8 A time source for the control plane

Section 2.1 is one instance of a general gap: `now_us` is threaded by hand
through every control-plane call, and one path forgot. The alternatives are a
timestamp on `ISysexIn`, or a `struct IClock { virtual uint32_t now_us() }`
seam that the control plane objects hold, as they hold `StatusLeds`. The
queue-and-dispatch approach in 2.1 is the smallest change that also fixes the
layering, because it makes SysEx take the same route as every other MIDI byte.

---

## 4. Simplification opportunities

### 4.1 Seven error enums, and the detail is lost at every hop

`ConfigError`, `LoadError`, `ParamError`, `CodecError`, `StoreError`,
`ApplyError` and `SysexError` each describe one layer, and each layer maps the
one below to a coarser code: `PatchManager` turns any `LoadError` into
`APPLY_INVALID`; the SysEx handler turns `PARAM_NO_SUCH_NODE`,
`PARAM_NO_SUCH_PARAM`, `PARAM_VALUE_OUT_OF_RANGE` and `PARAM_REFUSED` all into
`SYSEX_ERR_BAD_ARGUMENT` (`sysex_handler.cpp:170`). The detail then has to be
recovered through side channels: `registry::last_bad_param()` is a file-static
global (`registry.cpp:82`), `MixedModeMaster` keeps `error`, `node_error`,
`node_error_index` and `mapping_error_index`, and the console prints them as
raw numbers.

One `Diagnostic { layer, code, node, param }` value returned up the stack (or
carried in the NAK payload as `<layer> <code> <detail>`) would let the app say
"node 4, parameter 12 out of range" instead of "bad argument", and delete the
side channels. The NAK format can grow by appending, which the protocol's own
rule allows without a version bump.

### 4.2 `SysexHandler` owns things that are not SysEx

It holds the pending quantised swap (`pending_patch`, another 11.7 KB), the
swap timing, and `program_change()`, which is a MIDI channel message and lives
here only because that is where the swap state ended up. `PatchManager` is
documented as "the one path a patch takes to become the running graph"
(`patch_manager.h:10`) and is the natural owner: `PatchManager::recall(slot,
timing, now)` with the boundary logic, and `SysexHandler` shrinks to
framing, dispatch and replies. `timing` then disappears in favour of
`globals().pc_quantise`, which fixes 2.6's stale-timing case for free.

### 4.3 `PatchStore::occupied()` and `used()` decode a whole slot to answer a byte

Both are called per slot by `reply_slots` and `cmd_slots`. A `probe(slot)`
that reads the 8-byte header and checks the CRC over the payload (a few hundred
bytes through `crc16`) answers "occupied, and how big" without a `Patch` on the
stack, and `load()` can decode into a store-owned scratch and copy out only on
success.

### 4.4 The `-Weffc++` tax

Because of `-Weffc++`, every class lists every member in its initialiser list,
which is why constructors like `NoteSequencerBase`'s run to 25 lines of
`x(0), y(0)`. With default member initialisers (`uint8_t cursor = 0;`) the
constructors shrink to what they actually compute. This is mechanical and
worth doing alongside 3.1.

### 4.5 Macros to `constexpr`

`config.h` and the protocol headers use `#define` for every constant. The app's
generator parses `#define NAME value` and `enum Name : uint8_t` and nothing else
(`app/tools/generate-protocol.mjs:30`), so a move to `constexpr` needs a
matching change there, but it removes the `(uint16_t)` casts sprinkled through
the arithmetic and lets `bus_count()` and friends be `constexpr` too.

### 4.6 Documentation as code: the headers repeat the README

The design essays in the headers are excellent, and they are also the second
copy of the README. Where they have drifted (section 2.6), the header is
usually the accurate one. The cheapest fix is to make the README the index and
point at the headers, rather than restating them.

---

## 5. Scalability

### 5.1 `N_PARAM` is the shape that limits growth, and `Patch` is copied by value

`sizeof(NodeConfig)` is 350 bytes because `N_PARAM = 336` is set by the widest
node. `sizeof(Patch)` is **11 690 bytes**, and there are at least six live
copies: `PatchManager::live`, `::stage`, `SysexHandler::pending_patch`, the
emulator's `patch`, plus the stack temporaries in 2.5, and the pool's 21 KB.
About 90 KB of a 1 MB part, fine today. But the design couples three knobs:

- raising `MAX_SEQUENCE_LEN` to 64 doubles `N_PARAM`, doubles every copy, and
  halves how many sequencers fit an EEPROM slot;
- every logic gate carries 336 zero bytes in RAM that the codec then spends
  time trimming;
- the NRPN address space is `N_NODE * N_PARAM` (`nrpn.h:28`) and is already at
  `0x2A00` of `0x4000`: `N_PARAM` cannot pass 512 without redesigning the
  address map.

The scalable shape is a variable-length parameter block: `Patch` holds a byte
arena and each `NodeConfig` an offset and a length (its descriptor's
`n_params`). The codec already thinks this way; only the in-RAM struct does not.
If that is too large a change, the intermediate step is to stop copying:
`PatchManager` keeps `live` and lets callers edit through it, and
`SysexHandler` stages into the existing byte image rather than a decoded
`Patch`.

### 5.2 Preset slots: 1071 bytes each, and a full grid is a third of one

`EEPROM_BYTES / PATCH_SLOTS` gives 1071 bytes. A `PolySequencer` or a
`DrumSeqMidi` with a full grid encodes to about 350 bytes, so two of them plus
a few small nodes is already close to the ceiling, and three do not fit. The
trimming only removes *trailing* zeros; a drum grid is mostly zeros in the
middle. Two cheap wins before microSD: run-length encode zero runs inside a
parameter block (drum and poly grids compress five to ten times), and let
slots be variable-sized (a small allocation table at the head of the EEPROM)
so one large preset can borrow from three small ones. Both keep the "wire
format is the storage format" property if the RLE is part of the codec.

### 5.3 Fan-in depth and burst handling

Section 2.2 is the correctness half; the scalability half is that the note
bus depth is per pass and the pass rate is whatever the loop manages. A busy
patch (four MIDI inputs, chords, an arpeggiator and a drum sequencer on one
bus) can legitimately produce more than 16 events in a pass. Depth 16 costs
1.1 KB for the whole `BusManager`; depth 64 costs 4.5 KB. Raise it once the
drain is back-pressured, and expose `note_room()` so nodes that emit many
events (`Chord`, `DrumSeqMidi`) can refuse early instead of overflowing.

### 5.4 The control plane runs inline in the signal loop

`loop()` runs the transports, the drain, the pass, then the console, the
protocol and the store, all synchronously. Two things in that tail can take a
long time:

- `reply_dump()` sends up to ten 130-byte SysEx chunks in one call. Over DIN at
  31 250 baud that is 40 ms of serial per chunk, and `HardwareSerial::write`
  blocks when its transmit buffer (a few dozen bytes) is full. **A dump
  requested over DIN stalls every gate output for hundreds of milliseconds.**
  USB is better but `usbMIDI.sendSysEx` can also block when the host is slow.
- `PatchStore::save()` writes up to a kilobyte to emulated EEPROM, which on the
  Teensy 4.1 is a flash sector operation. It is debounced, but when it fires it
  fires inside the loop.

Neither is a problem in the tests or the emulator because both fakes are
instantaneous. The fix is a transmit queue for SysEx replies drained a chunk
per loop by `service()`, and the same "one chunk per loop" for the EEPROM
write. There is also no measurement of pass jitter at all; a `max pass us`
counter on the console would make every claim in the README about timing
checkable on hardware.

### 5.5 Pulse loss under subtick collapsing

`MasterClock::consume()` collapses subticks that arrive between two passes
into one `tick()` with the newest count, and the README says nothing is lost.
Position is exact, but `ClockDiv::advance_to` fires at most once per call and
skips whole periods (`clock_div.cpp:151`), so a multiplier whose period is one
or two subticks (×24, ×12 at high tempo) loses pulses whenever the loop is
slower than the subtick interval (350 µs at 300 BPM). With a 5 ms trigger
width the pulses would merge anyway, so it is a documentation gap more than a
bug, but the README's "exact" should say "exact in phase, bounded in count by
the pass rate".

### 5.6 Adding an algorithm touches too many places

Today a new algorithm means: the class, its descriptor, three name tables,
the `ParamGroup` table, `set_param`/`get_param`, an `AlgorithmId`, a line in
`registry.cpp`'s `TABLE`, and, for a sequencer, a case in the emulator's
`seq_kind()` switch (`emu_api.cpp:223`) and a view in the app. Sections 3.1
and 3.3 remove the two largest items. For the emulator, a `family` byte on the
descriptor (`gate sequencer`, `note sequencer`, `drum`, `modifier`, …) removes
the switch and lets the app pick a view without a hard-coded id list, which is
the same argument the README makes for names over indices.

### 5.7 Protocol headroom

The 7-bit encoding is handled by hand per message: an eighth bit as an optional
extra argument, a port-mask bit borrowed from a flags byte, `u14` for ranges.
It works and is documented, but each new field is a new special case. Two
structural limits are close: `SYSEX_TX_MAX = 320` with silent truncation
(2.4), and the NRPN map (5.1). Before the protocol gains more commands it is
worth adding one generic `SYSEX_GET_FIELD`/`SET_FIELD` pair addressed by
`(kind, index, offset, length)` with packed payloads, of which `GET_PATTERN`
and `GET_CONTROL` are already special cases.

### 5.8 Test build time

`test_build_src = yes` compiles all of `src/` fourteen times, once per test
directory. It is 16 s today; it grows linearly with both axes. Building the
core once as a PlatformIO library (`lib/mmmc`) and linking each test against
it is a config change.

---

## 6. A suggested order of work

Each step is independent of the ones after it, and the first four are small.

1. **Give SysEx a clock** (2.1, 3.8). Queue complete SysEx messages and
   dispatch from `loop()` with `now`. Add the three probes from 2.1 to
   `test_sysex` so the suite runs the handler at realistic uptimes.
2. **Back-pressure the MIDI drain** (2.2, 5.3), then raise
   `NOTE_QUEUE_DEPTH`. Add the 20-note-off probe to `test_master`.
3. **One owner for clock settings** (2.3): console through `set_globals`.
4. **NAK on reply overflow** (2.4); **static scratch `Patch` and `probe()`**
   (2.5, 4.3).
5. **Move preset recall and the quantised swap into `PatchManager`** (4.2);
   delete `SysexHandler::timing`.
6. **Extract `ControlTarget`** from `CcMapper` (3.7).
7. **Descriptor-driven parameter storage in `Node`** (3.1) with default member
   initialisers (4.4), then **bus indices in the base** (3.2). This is the
   large refactor; do it algorithm by algorithm behind the existing tests,
   which cover every `set_param` path.
8. **`NoteModifier` base** (3.3), `EdgeIn` everywhere (3.4), the root-follower
   and width helpers (3.5, 3.6).
9. **Unified diagnostics** (4.1) and the NAK payload extension.
10. **Variable-length parameter blocks** (5.1) and zero-run encoding (5.2),
    when a second grid-sized node or a longer sequence is actually wanted.
11. Housekeeping: dead code, include guards, README drift (2.6), a pass-jitter
    counter (5.4).
