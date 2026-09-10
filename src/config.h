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
#define N_NOTE_BUS 8
#define N_CV_BUS 8
// Events a note bus can carry per pass. Beyond this, writes are dropped and
// counted (BusManager::note_overflows()). The input drain never delivers
// past it (control/midi_dispatch.h), so the depth only has to cover what the
// nodes themselves emit in one pass: a four-voice PolySequencer and an
// eight-lane DrumSeqMidi on one bus is up to 24 note-ons and note-offs on one
// step. 32 costs 1 KB more than 16 across both buffers of eight buses.
#define NOTE_QUEUE_DEPTH 32

// Node pool: uniform slots, each large enough for any algorithm's state.
//
// The slot is sized by the two largest nodes, PolySequencer and DrumSeqMidi
// (#13, #14): a 32-step grid of velocities plus the note-off ledger. Every
// node class checks itself against it with a static_assert, so raising
// MAX_SEQUENCE_LEN or NOTE_SEQ_VOICES fails here at compile time rather than
// on the module. 40 x 640 bytes is 25 KB against 1 MB of RAM.
//
// **It was 32, and 32 stopped being enough when the algorithm table passed
// it.** A patch could no longer hold one of every algorithm, which is a test
// this repository has always run, and more to the point a generative patch
// built out of the module's own parts - a clock, two dividers, a harmony, a
// chord, a shift register, a quantiser, a comparator, a drum grid, a handful
// of logic and a modulator each - reaches the high twenties before anything
// interesting has been added to it.
//
// The ceiling is not RAM, it is the NRPN address space: node parameters
// occupy N_NODE x N_PARAM of the fourteen bits an NRPN address has, and the
// clock and transport blocks sit above them (control/nrpn.h). 40 x 336 is
// 13440 and leaves 2912 addresses reserved; 48 would leave 224, which is not
// enough room to add anything. So 40, and the next rise is a decision about
// that address space rather than about memory.
#define N_NODE 40
#define NODE_SLOT_SIZE 640

// Per-node connection limits (NodeConfig is also the preset format).
//
// N_PARAM is set by the widest parameter block: PolySequencer and DrumSeqMidi
// both carry a 16-byte header plus 320 bytes of steps (see
// algorithm/sequencer/note_sequencer.h and drum_sequencer.h). Every other
// algorithm uses the first few bytes and leaves the rest zero, which the
// patch protocol (#11) can exploit by not sending trailing zeros.
// MAX_IN went from 4 to 5 for the note sequencers' step-record inlets (#22):
// advance, reset, root, record and record-enable is five, and a sequencer
// that could not be played into would make step-record a host-only feature.
// It costs one byte per NodeConfig - 32 bytes of RAM and one byte per node on
// the wire - and gives the logic gates a fifth input for free.
#define MAX_IN 5
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

// Incoming MIDI events buffered between transport reads and the pass that
// consumes them (#5). A busy DIN port carries ~1000 status+data bytes per
// second, so ~350 messages/s; five transports at once and a 1 kHz pass rate
// still leaves this an order of magnitude of headroom.
#define MIDI_INPUT_QUEUE_DEPTH 64

#endif
