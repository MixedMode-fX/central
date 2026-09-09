#ifndef MMMC_ALGORITHM_GATE_TO_NOTE_H
#define MMMC_ALGORITHM_GATE_TO_NOTE_H

#include "node/node.h"

// Gate to MIDI note: a rising edge on the gate inlet sends note on, a
// falling edge sends note off.
//
// params[0] note (0 -> 60)   params[1] velocity (0 -> 100)   params[2] channel (0 -> 1)
class GateToNote : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit GateToNote(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;

    private:
        uint8_t in;
        uint8_t out;
        uint8_t note;
        uint8_t velocity;
        uint8_t channel;
        bool last;
};

#endif
