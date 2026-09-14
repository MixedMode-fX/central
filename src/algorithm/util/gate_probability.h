#ifndef MMMC_ALGORITHM_UTIL_GATE_PROBABILITY_H
#define MMMC_ALGORITHM_UTIL_GATE_PROBABILITY_H

#include "node/node.h"
#include "algorithm/sequencer/step_engine.h"
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
// object Probability carries, so a hat and a melody set to 2:4 fall on the
// same beats and either one's `passed` can be read by the other.
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
// **`dropped` is what the rule took out**, the gate side of Probability's
// third outlet: it is high for exactly as long as an input gate whose edge
// was refused, and low for one that was passed, so `gate out` and `dropped`
// are the same pulse train split in two. A kick on one and a hat on the
// other interlock by construction, and raising the chance moves the hits
// from one to the other rather than making holes.
//
// It says in one outlet what `NOT(passed)` and an `AND` with this node's own
// input already say in two nodes and a bus, which is normally the argument
// against having it. It is here because the pair is one rule asked of two
// signals (algorithm/util/trig_condition.h): the note side cannot be built
// from cables at all - nothing downstream can tell a dropped note-on from
// one that was never played - and an outlet that exists on one half and not
// the other would make the two nodes two algorithms. `passed` is untouched
// and is still what a *neighbour* rule reads: it is another node's decision
// that no outlet here can carry.
//
// Inlet 0 (gate): gate in.
// Inlet 1 (gate, optional): reset. A rising edge returns the count to the
//         top, exactly as it does on every sequencer in the module.
// Outlet 0 (gate): gate out.
// Outlet 1 (gate): dropped - the gates the rule refused, held for their
//         own length.
// Outlet 2 (gate): passed - this node's last decision, latched. `AND` it
//         with another node's input to play only where this one played, and
//         put a `NOT` in front to play only where it did not.
//
// params[0..2] are TrigCondition's block: chance, condition, seed.
class GateProbability : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit GateProbability(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        bool open() const { return passing; }
        bool blocked() const { return last_in && !passing; }
        bool passed() const { return condition.passed(); }

    private:
        uint8_t in;
        uint8_t out;
        uint8_t dropped_out;
        uint8_t passed_out;
        EdgeIn reset_in;
        TrigCondition condition;
        bool last_in;
        bool passing;           // what the edge decided, held until it falls
};

#endif
