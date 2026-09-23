#ifndef MMMC_CONFIG_H
#define MMMC_CONFIG_H

// Framework-free sizing constants shared by the firmware and the native tests.
// Anything that names a physical pin or a transport lives in hardware.h, which
// is only ever included by the Teensy hardware layer (src/hal/teensy/).
//
// See README "Signal bus model" for the reasoning behind these numbers.

#include <stdint.h>

// Number of front-panel I/O ports (jacks).
#define GPIO_N 8
// Bitmask selecting every port.
#define ALL_GPIO_MAP ((uint16_t)((1u << GPIO_N) - 1u))

// Internal buses per domain.
#define N_GATE_BUS 16
#define N_NOTE_BUS 16
#define N_CV_BUS 8
// Events a note bus can carry per pass. Beyond this, writes are dropped and
// counted (BusManager::note_overflows()). The input drain never delivers
// past it (control/midi_dispatch.h), so the depth only has to cover what the
// nodes themselves emit in one pass: a four-voice PolySequencer and an
// eight-lane DrumSeqMidi on one bus is up to 24 note-ons and note-offs on one
// step. 32 costs 2 KB more than 16 across both buffers of every note bus.
#define NOTE_QUEUE_DEPTH 32

// Node pool: uniform slots, each large enough for any algorithm's state.
//
// The slot is sized by the two largest nodes, PolySequencer and DrumSeqMidi
// (#13, #14): a 32-step grid of velocities plus the note-off ledger. Every
// node class checks itself against it with a static_assert, so raising
// MAX_SEQUENCE_LEN or NOTE_SEQ_VOICES fails here at compile time rather than
// on the module. 45 x 640 bytes is 28 KB against 1 MB of RAM.
//
// **The pool is what a patch can hold, not the table.** The table is wider
// than the pool, so a patch cannot hold one of every algorithm - and no patch
// wants to; test_master runs the whole table through the pool in batches of
// this many instead. What the number has to cover is a generative patch built
// out of the module's own parts - a clock, two dividers, a harmony, a chord,
// a shift register, a quantiser, a comparator, a drum grid, a handful of
// logic and a modulator each - which reaches the high twenties before
// anything interesting has been added to it, and a song out of two of those
// behind a switch is most of the rest.
//
// The ceiling is not RAM, it is the NRPN address space: node parameters
// occupy N_NODE x N_PARAM of the fourteen bits an NRPN address has, and the
// clock, transport and key blocks sit above them (control/nrpn.h). 46 x 336
// is 15456 and leaves 880 addresses reserved; 48 would leave 208, which is
// not enough room to add anything. So 46 is most of what the layout has left,
// and the next rise is a decision about that address space rather than about
// memory: N_PARAM has to come down, or the space has to be paged.
//
// Raising this moves the NRPN bases, and the protocol version moves with
// them. control/nrpn.cpp asserts the two agree.
#define N_NODE 46
#define NODE_SLOT_SIZE 640

// Per-node connection limits (NodeConfig is also the preset format).
//
// N_PARAM is set by the widest parameter block: PolySequencer and DrumSeqMidi
// both carry a 16-byte header plus 320 bytes of steps (see
// algorithm/sequencer/note_sequencer.h and drum_sequencer.h). Every other
// algorithm uses the first few bytes and leaves the rest zero, which the
// patch protocol (#11) can exploit by not sending trailing zeros.
// MAX_IN is set by the switches (algorithm/switch/gate_switch.h): a many-to-one
// switch spends three inlets on choosing - select, step and reset - and what
// is left is how many parts it can choose between, so eight is five parts,
// the same width MAX_OUT gives the one-to-many routers. It costs two bytes
// per inlet per NodeConfig in RAM; on the wire and in EEPROM a port list is
// trimmed at the last connected inlet, so a node using two pays for two.
// The note sequencers need five (advance, reset, root, record, record
// enable) and the logic gates fold over every one they are given.
#define MAX_IN 8
#define MAX_OUT 8
#define N_PARAM 336

// Reserved hardware nodes outside the pool.
#define N_MIDI_IN_NODES 4
#define N_MIDI_OUT_NODES 4

// Master clock (#4).
//
// The module's musical resolution is MASTER_PPQN, the figure the README
// specifies. The timer runs faster than that - CLOCK_SUBTICK subticks per
// PPQN tick - because a multiplier can only be exact if the tick it
// multiplies is already subdivided, and the subdivision is what caps the
// fastest multiplier: x N is exact only when N divides CLOCK_SUBTICK.
//
// CLOCK_SUBTICK = 24 was chosen for its divisors: x2, x3, x4, x6, x8, x12
// and x24 are all exact, so triplets are as exact as duplets. That puts the
// timer at MASTER_PPQN * CLOCK_SUBTICK = 576 interrupts per quarter note:
// 1.2 kHz at 120 BPM and 2.9 kHz at 300 BPM, for an ISR that increments one
// counter. A power-of-two subdivision would be cheaper still and could not
// express a triplet.
#define MASTER_PPQN 24
#define CLOCK_SUBTICK 24
#define CLOCK_SUBTICKS_PER_QUARTER (MASTER_PPQN * CLOCK_SUBTICK)

// Tempo limits for the internal clock, and the default at boot.
#define CLOCK_MIN_BPM 20
#define CLOCK_MAX_BPM 300
#define CLOCK_DEFAULT_BPM 120

// Trigger pulse width, in microseconds: fixed, never a function of tempo.
// Eurorack triggers are 1-10 ms; 5 ms is comfortably inside that and still
// shorter than one subtick at the fastest tempo this module clocks.
#define TRIGGER_WIDTH_US 5000
#define TRIGGER_MIN_WIDTH_US 500
#define TRIGGER_MAX_WIDTH_US 50000

// Steps a sequencer can hold. The README said 16, inherited from hardware
// that does not constrain us; 32 fits a pattern in one word and, at four
// voices, keeps the largest sequencer under NODE_SLOT_SIZE (#13).
#define MAX_SEQUENCE_LEN 32

// Voices a PolySequencer step can hold (#13). Four is a chord; each voice
// costs two bytes per step, so this is the other knob on NODE_SLOT_SIZE.
#define NOTE_SEQ_VOICES 4

// Lanes in a drum sequencer (#14). Eight is one lane per jack, and the gate
// variant has one outlet per lane, so this cannot exceed MAX_OUT.
#define DRUM_SEQ_LANES 8

// Notes one modifier can hold at once (#10). Ten fingers plus headroom for
// notes still sounding under a sustain pedal, and 16 keeps HeldNotes at
// 64 bytes.
#define MAX_HELD_NOTES 16

// Notes one modifier can have sounding at once (#10). A modifier owns the
// note-off for every note-on it emitted, so it can only emit what it can
// record: Chord turns one held note into several, and this is the ceiling on
// the total. An emission that would not fit is refused rather than left
// unreleasable.
#define MAX_SOUNDING_NOTES 32

// Controller bindings a patch can carry (#21).
//
// Thirty-two is a controller's worth - every knob and fader on a typical
// 16-knob box, twice over - and costs 32 x sizeof(CcMapping) = 384 bytes of
// RAM inside Patch. Unused entries are not stored or transmitted at all
// (patch_codec trims them), so a patch with no mappings pays nothing on the
// wire or in EEPROM.
#define N_CC_MAP 32

// Modulation routes a patch can carry.
//
// A route binds one CV bus to one parameter (node/patch.h, control/mod_matrix.h).
// Sixteen is two per CV bus, which is the shape that actually occurs - one
// modulator reaching several parameters - and costs 16 x sizeof(ModRoute) =
// 160 bytes of RAM inside Patch. Unused entries are not stored or transmitted
// at all (patch_codec trims them), so a patch with no modulation pays nothing
// on the wire or in EEPROM.
#define N_MOD_ROUTE 16

// Macros a patch can carry, and the destinations they share.
//
// A macro is one performance control that moves several parameters at once,
// each over its own window of the macro's travel (node/patch.h,
// control/macros.h). Eight is this machine's idiom - GPIO_N and N_CV_BUS are
// both 8 - and one pot row on every controller worth copying.
//
// The destinations are a **shared pool**, not a fixed array per macro,
// because real macros are lopsided: one sweeping control with six
// destinations and three with one each. A fixed array would charge the
// one-destination macro the same as the six. Thirty-two matches N_CC_MAP and
// caps the per-pass cost at 32 contributions; N_MACRO_DEST_PER_MACRO stops
// one macro eating the pool. Unused entries are not stored or transmitted
// (patch_codec trims them).
#define N_MACRO 8
#define N_MACRO_DEST 32
#define N_MACRO_DEST_PER_MACRO 8

// A macro's name, fixed width and not length-prefixed. The patch codec is a
// hand-rolled byte reader mirrored by hand in app/src/protocol/codec.js, and
// a memcpy is the one form that cannot disagree between the two. Eight
// characters is also about what a panel display will give you, so the limit
// is real rather than arbitrary.
#define MACRO_NAME_BYTES 8

// Incoming MIDI events buffered between transport reads and the pass that
// consumes them (#5). A busy DIN port carries ~1000 status+data bytes per
// second, so ~350 messages/s; five transports at once and a 1 kHz pass rate
// still leaves this an order of magnitude of headroom.
#define MIDI_INPUT_QUEUE_DEPTH 64

// The monitor (monitor/monitor.h): what an editor is told about the running
// module, once per request.
//
// MONITOR_EVENTS is the note-ons and note-offs kept between two requests,
// on the watched buses and on the cables together. An editor asks once per
// animation frame, so this covers a burst of a few passes' worth - a chord
// and a drum step on one frame - and a burst past it is reported as lost
// rather than stalling a pass. MONITOR_MAX_NODES is how many nodes one
// request may ask the position of: a playhead is drawn for an open card, and
// eight open cards is a tall screen. MONITOR_NODE_VALUES is the widest
// answer for one of them - a harmony's degree and loop slot, and then a full
// loop of chords (algorithm/midi/harmony.h, MAX_PHRASE). Together they
// bound the frame, which is what sizes SYSEX_TX_MAX.
#define MONITOR_EVENTS 32
#define MONITOR_MAX_NODES 8
#define MONITOR_NODE_VALUES 18
// A request arms the monitor for this long. A module nobody is asking does
// nothing here, and one whose editor went away is back to that a second
// later with nothing to tear down.
#define MONITOR_ARMED_US 1000000u

#endif
