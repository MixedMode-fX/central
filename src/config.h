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
// counted (BusManager::note_overflows()).
#define NOTE_QUEUE_DEPTH 16

// Node pool: uniform slots, each large enough for any algorithm's state.
#define N_NODE 32
#define NODE_SLOT_SIZE 320

// Per-node connection limits (NodeConfig is also the preset format).
#define MAX_IN 4
#define MAX_OUT 8
#define N_PARAM 8

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

// Steps a sequencer can hold. The README says 16, inherited from hardware
// that does not constrain us; 32 fits one word and #13 can raise it again.
#define MAX_SEQUENCE_LEN 32

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

// Incoming MIDI events buffered between transport reads and the pass that
// consumes them (#5). A busy DIN port carries ~1000 status+data bytes per
// second, so ~350 messages/s; five transports at once and a 1 kHz pass rate
// still leaves this an order of magnitude of headroom.
#define MIDI_INPUT_QUEUE_DEPTH 64

#endif
