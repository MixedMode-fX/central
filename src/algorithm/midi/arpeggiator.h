#ifndef MMMC_ALGORITHM_ARPEGGIATOR_H
#define MMMC_ALGORITHM_ARPEGGIATOR_H

#include "node/node.h"

// Minimal arpeggiator, enough to exercise the two-domain inlet contract of
// the bus model (#9). #10 extends it (modes, octaves, hold, gate length).
//
// Inlet 0 (note): the held chord - note on adds, note off removes.
// Inlet 1 (gate): advance - each rising edge releases the sounding note and
//                 plays the next held note in ascending order.
// Outlet 0 (note): the arpeggiated notes.
class Arpeggiator : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t MAX_HELD = 8;
        static constexpr uint8_t NONE = 0xFF;

        explicit Arpeggiator(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;

        uint8_t held_count() const { return count; }

    private:
        void add(uint8_t note, uint8_t velocity, uint8_t channel);
        void remove(uint8_t note);
        void release(BusManager& bus);

        uint8_t held_in;
        uint8_t advance_in;
        uint8_t out;
        uint8_t notes[MAX_HELD];       // ascending
        uint8_t velocities[MAX_HELD];
        uint8_t count;
        uint8_t index;
        uint8_t sounding;
        uint8_t channel;
        bool last_gate;
};

#endif
