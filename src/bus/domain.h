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

// ---------------------------------------------------------------------------
// What a port is connected to.
//
// **Every port names a set of buses, not a bus.** An outlet writes all of
// them, an inlet reads all of them, and the empty set is "not connected".
// That single rule is what makes the patch a graph rather than a chain:
//
//   * one outlet on a bus any number of inlets read is a fan-out, and always
//     was - a bus has never cared how many readers it has;
//   * an inlet reading two buses is a **merge**, under its domain's own
//     fan-in rule, and it costs its sources nothing: each keeps its own bus
//     and whatever else was already listening to it;
//   * an outlet on two buses is the same freedom from the writing end, for
//     the patch where a signal has to join two merges that are otherwise
//     unrelated.
//
// Before this, a port held one bus index, so two signals could only be summed
// by putting their sources on one bus - which silently merged everything else
// those sources were driving. Connecting anything could therefore disconnect
// something somewhere else, and that is the rigidity this replaces.
//
// A set is a bit per bus. Every domain has at most sixteen buses (the
// static_asserts below), so one is sixteen bits: two bytes per port in the
// preset format and in RAM.
// Plain data with no constructors of its own, so a NodeConfig holding one is
// still the aggregate the preset format treats it as: `BusSet{}` is the empty
// set and `BusSet{bits}` names one directly.
struct BusSet {
    uint16_t bits;

    bool any() const { return bits != 0; }
    bool has(uint8_t bus) const { return bus < 16 && (bits & (uint16_t)(1u << bus)) != 0; }
    void add(uint8_t bus){ if (bus < 16) bits = (uint16_t)(bits | (1u << bus)); }
    void remove(uint8_t bus){ if (bus < 16) bits = (uint16_t)(bits & ~(1u << bus)); }
    uint8_t count() const {
        uint8_t n = 0;
        for (uint16_t b = bits; b != 0; b &= (uint16_t)(b - 1u)) n++;
        return n;
    }
    // The lowest bus in the set, or 0xFF when it is empty. What a caller
    // wants when a set is known to hold at most one - a jack's direction
    // toggle, a console listing.
    uint8_t first() const {
        for (uint8_t b = 0; b < 16; b++) if (has(b)) return b;
        return 0xFF;
    }
};

inline bool operator==(BusSet a, BusSet b){ return a.bits == b.bits; }
inline bool operator!=(BusSet a, BusSet b){ return a.bits != b.bits; }

// The set holding one bus, which is what a single connection is.
inline BusSet one_bus(uint8_t bus){ BusSet s{}; s.add(bus); return s; }

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

// Every bus a domain has, as a set: what a port's set is checked against.
inline BusSet all_buses(Domain d){
    const uint8_t n = bus_count(d);
    return BusSet{(uint16_t)((n >= 16) ? 0xFFFFu : ((1u << n) - 1u))};
}

// True when every bus in `set` exists in `domain`. The validator's whole
// question about a port.
inline bool buses_in_range(Domain domain, BusSet set){
    return (set.bits & ~all_buses(domain).bits) == 0;
}

// What travels on a note bus: any channel-voice MIDI message, so CC, pitch
// bend, aftertouch and program change share the bus with notes (#5).
struct MidiEvent {
    uint8_t type;      // MidiType status nibble (0x80 .. 0xE0)
    uint8_t channel;   // 1 .. 16
    uint8_t data1;
    uint8_t data2;
};

static_assert(N_GATE_BUS <= 16, "a BusSet is sixteen bits");
static_assert(N_NOTE_BUS <= 16, "a BusSet is sixteen bits");
static_assert(N_CV_BUS <= 16, "a BusSet is sixteen bits");

#endif
