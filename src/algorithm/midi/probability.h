#ifndef MMMC_ALGORITHM_PROBABILITY_H
#define MMMC_ALGORITHM_PROBABILITY_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/sequencer/step_engine.h"
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
// byte for byte with GateProbability: a chance, and a condition on the count
// of events this node has seen. The node is what the rule is applied *to* -
// here, a note-on and the note-off that belongs to it.
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
// **`passed` is the decision, made patchable.** It carries the node's last
// answer, latched until the next note-on rather than pulsed, so a reader
// clocked in some other pass still sees it. That outlet is what a groovebox
// spends two conditions on: `AND(this node's input, another's passed)` is
// "only where that one played" and a NOT in front of it is "only where it
// did not", across both domains and any distance in the graph. There is no
// neighbour rule here because there is no neighbour - there is a cable.
//
// **`dropped` is what the rule took out**, and it is not the decision but the
// notes themselves: every refused note-on leaves by it unchanged, with its
// own note-off, so a thinned line is a split line rather than a line with
// holes in it. That is a second voice playing exactly where the first does
// not - a ghost part on another channel, the hits the snare refused sent to
// a hat - and it is the one thing here no cable can build. `passed` only
// reports the decision; a second Probability at the same odds would take its
// own decisions rather than the complement of these, and nothing downstream
// can tell a note that was dropped from a note that was never played.
//
// A message that is not a note leaves by `notes out` alone. It was never
// refused, so a copy of it on `dropped` would be a second stream of it, not
// the other half of a rule that was never asked about it.
//
// Inlet 0 (note): notes in.
// Inlet 1 (gate, optional): reset. A rising edge returns the count to the
//         top, exactly as it does on every sequencer in the module.
// Outlet 0 (note): notes out.
// Outlet 1 (note): dropped - the notes the rule refused, note-offs included.
// Outlet 2 (gate): passed - this node's last decision, latched.
//
// params[0..2] are TrigCondition's block: chance, condition, seed.
class Probability : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Probability(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t sounding_count() const { return sounding.count(); }
        uint8_t dropped_count() const { return refused.count(); }
        bool passed() const { return condition.passed(); }

    private:
        uint8_t in;
        uint8_t out;
        uint8_t dropped_out;
        uint8_t passed_out;
        EdgeIn reset_in;
        TrigCondition condition;
        SoundingNotes sounding;
        SoundingNotes refused;      // what left by `dropped`, and still owes its note-off
};

#endif
