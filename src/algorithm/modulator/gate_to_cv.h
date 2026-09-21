#ifndef MMMC_ALGORITHM_MODULATOR_GATE_TO_CV_H
#define MMMC_ALGORITHM_MODULATOR_GATE_TO_CV_H

#include "node/node.h"

// A gate as a level: `high` while the gate is up, `low` while it is down.
//
// The door from the gate domain into the control bus, and the last one the
// module was missing: MidiToCV turns a note into a level and CvToGate a
// level into a gate, but a gate on its own could reach no parameter. Now it
// can - a pad held down raising a sequencer's probability through the
// modulation matrix, a bar-long square from a Counter's bit swinging a delay
// - and it can reach a switch's `select` inlet, where a gate pattern from a
// StepSequencer at one bar per step is which part plays on which bar: high
// is the second part, low the first. That is the two-way switch with a
// pattern for a hand, and it is why the levels are the parameters rather
// than a fixed 0 and full scale.
//
// Inlet 0 (gate, required): the gate.
// Outlet 0 (CV): the level, every pass.
//
// params[0] low   the level while the gate is down, in percent of full scale
// params[1] high  the level while it is up. Below `low` inverts the gate.
class GateToCv : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint16_t P_LOW = 0, P_HIGH = 1;
        static constexpr uint8_t DEFAULT_HIGH = 100;

        explicit GateToCv(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        static int16_t level_of(uint8_t percent){
            return (int16_t)(((int32_t)percent * CV_MAX) / 100);
        }

    private:
        BusSet in;
        BusSet out;
        uint8_t low;
        uint8_t high;
};

#endif
