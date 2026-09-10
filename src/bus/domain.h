#ifndef MMMC_BUS_DOMAIN_H
#define MMMC_BUS_DOMAIN_H

#include <stdint.h>
#include "config.h"

// The three signal domains. Every inlet and outlet of every node belongs to
// exactly one, declared statically in the node's AlgorithmDescriptor, so a
// bus index in NodeConfig is always interpreted in the right space.
enum class Domain : uint8_t {
    Gate,   // a level (bool). Fan-in: OR of all writers.
    Note,   // a MIDI event stream. Fan-in: arrival order, overflow counted.
    CV,     // int16_t. Fan-in: sum with saturation. Modulators and #8's jacks.
};

// "Not connected" marker for an optional inlet.
#define NO_BUS 0xFF

// ---------------------------------------------------------------------------
// What a number on a CV bus means.
//
// The CV domain is the module's **internal control bus**: one value per bus
// per pass, summed on fan-in, published by the same swap as everything else.
// Modulators write it and the modulation matrix (control/mod_matrix.h) reads
// it to move parameters. It is not a note bus, and that is deliberate - a
// note bus carries MIDI events, whose data bytes are seven bits, so a
// modulator sent that way would arrive at 128 steps and would have to invent
// a controller number to be recognised by. A control signal is a *value*, not
// an event: it has a level every pass whether or not anything changed, which
// is exactly what a bus of int16_t already is.
//
// **Full scale is twelve bits, not seven.** Seven bits is 128 steps over a
// parameter's whole range, which is audible as stepping on anything a
// modulator is worth using for - a slow filter sweep, a pitch glide, a fine
// detune. Twelve gives 4096, which is finer than any parameter the module
// has (a parameter byte is 8 bits) and finer than the 12-bit DAC a jack would
// use, so the modulation matrix rounds *down* into the target's range rather
// than interpolating up from a coarser one.
//
// **Why twelve and not fourteen.** The bus is int16_t and fan-in is a sum, so
// full scale bounds how many modulators can share a bus before the sum
// saturates and stops being the mix a user asked for. At twelve bits,
// N_CV_BUS writers at full positive scale come to 32768 - one LSB past
// INT16_MAX - so eight modulators summed on one bus clip by a single step at
// the very top and nowhere else. At fourteen bits, two writers would clip.
//
// Unipolar signals run 0 .. CV_MAX. Bipolar signals run -CV_HALF .. +CV_HALF-1
// around zero; nothing in the bus treats the two differently, it is the
// node's parameter that says which it is producing and the matrix's flags
// that say how to read it.
#define CV_BITS 12
#define CV_FULL 4096              // one full scale, exclusive
#define CV_MAX  (CV_FULL - 1)     // 4095: the largest unipolar value
#define CV_HALF (CV_FULL / 2)     // 2048: bipolar runs -CV_HALF .. CV_HALF-1

// Clamps to the unipolar range. Modulator outputs go through this, so a depth
// and an offset that would together leave the range are limited rather than
// wrapping - a modulator that wrapped from the top back to the bottom would
// put a step in the middle of a sweep.
inline int32_t cv_clamp_unipolar(int32_t v){
    if (v < 0) return 0;
    if (v > CV_MAX) return CV_MAX;
    return v;
}

// Clamps to the bipolar range.
inline int32_t cv_clamp_bipolar(int32_t v){
    if (v < -CV_HALF) return -CV_HALF;
    if (v > CV_HALF - 1) return CV_HALF - 1;
    return v;
}

inline uint8_t bus_count(Domain d){
    switch (d){
        case Domain::Gate: return N_GATE_BUS;
        case Domain::Note: return N_NOTE_BUS;
        default:           return N_CV_BUS;
    }
}

// What travels on a note bus: any channel-voice MIDI message, so CC, pitch
// bend, aftertouch and program change share the bus with notes (#5).
struct MidiEvent {
    uint8_t type;      // MidiType status nibble (0x80 .. 0xE0)
    uint8_t channel;   // 1 .. 16
    uint8_t data1;
    uint8_t data2;
};

#endif
