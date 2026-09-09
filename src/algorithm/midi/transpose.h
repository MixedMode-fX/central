#ifndef MMMC_ALGORITHM_TRANSPOSE_H
#define MMMC_ALGORITHM_TRANSPOSE_H

#include "node/node.h"

// Shifts every note on/off by a number of semitones; other events pass
// through unchanged. A note shifted outside 0..127 is dropped (both its on
// and its off, so nothing hangs).
//
// params[0] semitones, as a signed 8-bit value stored in the byte
class Transpose : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Transpose(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;

    private:
        uint8_t in;
        uint8_t out;
        int8_t semitones;
};

#endif
