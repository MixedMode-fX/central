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
    CV,     // int16_t. Fan-in: sum with saturation. Reserved for #8.
};

// "Not connected" marker for an optional inlet.
#define NO_BUS 0xFF

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
