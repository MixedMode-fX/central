#ifndef MMMC_ALGORITHM_UTIL_GATE_PROBABILITY_H
#define MMMC_ALGORITHM_UTIL_GATE_PROBABILITY_H

#include "node/node.h"
#include "algorithm/util/trig_condition.h"

// Lets each gate through under a condition. Probability, for the other half
// of the module.
//
// Everything that made a probability worth a node of its own on the note side
// is true of gates, and there was no way to ask for it: a clock, a divider, a
// Euclidean pattern and a drum lane all write gate buses, and the only way to
// thin one was to send it through GateToNote, a Probability and back, which
// costs three nodes, a note bus and a channel to carry a decision that was
// never about pitch.
//
// The rule is TrigCondition (algorithm/util/trig_condition.h), the same
// object Probability carries - chance, an X:Y ratio over the events that
// reach the node, and a condition - so a hat and a melody set to 2:4 fall on
// the same beats, and either one's `passed` can drive the other's `nei`.
//
// **What an event is here is a rising edge**, and the decision is held for
// the whole of the gate it was taken on: a gate that was passed stays up for
// exactly as long as the input does, and one that was dropped stays down for
// exactly as long, rather than flickering as the dice are re-rolled every
// pass. That is the same rule the note side keeps by pairing a note-off with
// its note-on, said in the vocabulary of a level.
//
// A trigger is a gate that happens to be short, so nothing here knows the
// difference: the module's 5 ms triggers pass through whole or not at all.
//
// Inlet 0 (gate): gate in.
// Inlet 1 (gate, optional): fill. High is `fill`, low is `not fill`.
// Inlet 2 (gate, optional): nei. The neighbour's last decision, usually
//         another probability node's `passed`.
// Outlet 0 (gate): gate out.
// Outlet 1 (gate): passed - this node's last decision, latched.
//
// params[0..3] are TrigCondition's block: chance, ratio, condition, seed.
class GateProbability : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit GateProbability(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void transport_event(BusManager& bus, uint8_t edges) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        bool open() const { return passing; }
        bool passed() const { return condition.passed(); }

    private:
        uint8_t in;
        uint8_t fill_in;
        uint8_t nei_in;
        uint8_t out;
        uint8_t passed_out;
        TrigCondition condition;
        bool last_in;
        bool passing;           // what the edge decided, held until it falls
};

#endif
