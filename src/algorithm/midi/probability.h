#ifndef MMMC_ALGORITHM_PROBABILITY_H
#define MMMC_ALGORITHM_PROBABILITY_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "util/random.h"

// Lets each note through with a probability, and keeps its note-off with it.
//
// "Randomiser" is four different algorithms wearing one name - random note
// choice, random velocity, timing humanisation, and chance of passing an
// event - and a single node with a mode switch would be worse than any of
// them. Note choice belongs to the arpeggiator's random mode, velocity to
// VelocityCurve, and humanisation needs a delay line this firmware does not
// have yet. What earns its own node is the fourth: probability per event,
// which is what makes a repeating pattern breathe.
//
// The decision is taken once, on the note-on, and remembered: a note-off
// whose note-on was dropped is dropped too, and a note-off whose note-on was
// passed is always passed, however the dice fall afterwards. A probability
// applied to note-offs independently is how a sequencer hangs a synth.
//
// params[0] chance, percent (0 -> 100, so an unconfigured node passes
//                   everything rather than silencing the patch)
// params[1] seed offset, so two nodes at the same odds do not agree
class Probability : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Probability(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t sounding_count() const { return sounding.count(); }

    private:
        uint8_t in;
        uint8_t out;
        uint8_t percent;
        uint8_t seed_offset;
        Xorshift32 rng;
        SoundingNotes sounding;
};

#endif
