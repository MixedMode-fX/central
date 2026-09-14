#ifndef MMMC_ALGORITHM_PROBABILITY_H
#define MMMC_ALGORITHM_PROBABILITY_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/util/trig_condition.h"

// Lets each note through under a condition, and keeps its note-off with it.
//
// "Randomiser" is four different algorithms wearing one name - random note
// choice, random velocity, timing humanisation, and chance of passing an
// event - and a single node with a mode switch would be worse than any of
// them. Note choice belongs to the arpeggiator's random mode, velocity to
// VelocityCurve, and humanisation needs a delay line this firmware does not
// have yet. What earns its own node is the fourth: whether an event happens,
// which is what makes a repeating pattern breathe.
//
// The rule itself is TrigCondition (algorithm/util/trig_condition.h), shared
// byte for byte with GateProbability: a chance, an X:Y ratio over the events
// that reach the node, and a condition. The node is what the rule is applied
// *to* - here, a note-on and the note-off that belongs to it.
//
// The decision is taken once, on the note-on, and remembered: a note-off
// whose note-on was dropped is dropped too, and a note-off whose note-on was
// passed is always passed, however the dice fall afterwards. A probability
// applied to note-offs independently is how a sequencer hangs a synth.
//
// Anything that is not a note - a CC, a bend, aftertouch - is passed through
// untouched and does not advance the count. The rule is about notes; a
// modulation stream thinned at random is a different algorithm and nobody
// asked for it.
//
// Inlet 0 (note): notes in.
// Inlet 1 (gate, optional): fill. High is `fill`, low is `not fill`.
// Inlet 2 (gate, optional): nei. The neighbour's last decision, usually
//         another Probability's `passed`.
// Outlet 0 (note): notes out.
// Outlet 1 (gate): passed - this node's last decision, latched.
//
// params[0..3] are TrigCondition's block: chance, ratio, condition, seed.
class Probability : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Probability(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        void transport_event(BusManager& bus, uint8_t edges) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t sounding_count() const { return sounding.count(); }
        bool passed() const { return condition.passed(); }

    private:
        uint8_t in;
        uint8_t fill_in;
        uint8_t nei_in;
        uint8_t out;
        uint8_t passed_out;
        TrigCondition condition;
        SoundingNotes sounding;
};

#endif
