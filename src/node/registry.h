#ifndef MMMC_NODE_REGISTRY_H
#define MMMC_NODE_REGISTRY_H

#include <stdint.h>
#include "node/node.h"

// Every algorithm compiled into this firmware, by id. Ids are part of the
// preset format: never renumber, only append.
enum AlgorithmId : uint8_t {
    ALGO_NONE         = 0,
    ALGO_LOGIC_NOT    = 1,
    ALGO_LOGIC_AND    = 2,
    ALGO_LOGIC_NAND   = 3,
    ALGO_LOGIC_OR     = 4,
    ALGO_LOGIC_NOR    = 5,
    ALGO_LOGIC_XOR    = 6,
    ALGO_LOGIC_XNOR   = 7,
    ALGO_SUSTAIN      = 8,
    ALGO_GATE_TO_NOTE = 9,
    ALGO_TRANSPOSE    = 10,
    ALGO_ARPEGGIATOR  = 11,
    ALGO_CLOCK_DIV    = 12,
    ALGO_METRONOME    = 13,
    ALGO_STEP_SEQ     = 14,
    ALGO_EUCLID_SEQ   = 15,
    ALGO_RANDOM_SEQ   = 16,
    ALGO_NOTE_PRIORITY = 17,
    ALGO_VELOCITY     = 18,
    ALGO_CHORD        = 19,
    ALGO_QUANTISE     = 20,
    ALGO_PROBABILITY  = 21,
    ALGO_NOTE_SEQ     = 22,
    ALGO_POLY_SEQ     = 23,
    ALGO_DRUM_SEQ_GATE = 24,
    ALGO_DRUM_SEQ_MIDI = 25,
};

enum ConfigError : uint8_t {
    CONFIG_OK = 0,
    CONFIG_UNKNOWN_ALGORITHM,
    CONFIG_INLET_OUT_OF_RANGE,     // bus index not valid for that inlet's domain
    CONFIG_INLET_NOT_CONNECTED,    // required inlet is NO_BUS
    CONFIG_OUTLET_OUT_OF_RANGE,    // an outlet may be NO_BUS (unused), never out of range
};

namespace registry {
    // nullptr if the id is unknown.
    const AlgorithmDescriptor* find(uint8_t algorithm_id);
    // Iteration, for the editor (#11) and the tests.
    uint8_t count();
    const AlgorithmDescriptor* at(uint8_t index);
    // Range-checks every bus index against its inlet's / outlet's domain.
    ConfigError validate(const NodeConfig& config);
}

#endif
