#ifndef MMMC_ALGORITHM_SEQUENCER_SEQUENCERS_H
#define MMMC_ALGORITHM_SEQUENCER_SEQUENCERS_H

#include "algorithm/sequencer/gate_sequencer.h"

// The README's four gate sequencers (#6). Each writes a bool to a gate bus
// and nothing else: a gate bus cannot carry pitch, velocity, note length or
// polyphony, so note sequencing (#13) and drum sequencing (#14) are separate
// families that share the step engine underneath (step_engine.h).

// Every advance edge is output. GateSequencer with a length of one, and the
// clearest demonstration that the divider upstream is what sets the rate.
class Metronome : public GateSequencer{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Metronome(const NodeConfig& config) : GateSequencer(config, 1) {}
    protected:
        bool step_on(uint8_t) const override { return true; }
};

// A pattern of on/off steps, held as a bitfield.
//
// params[3..6] the pattern, step 0 in the low bit of params[3], so all
//              MAX_SEQUENCE_LEN steps fit in four bytes of the preset.
//
// Steps beyond the current length are stored but not played, so shortening
// and lengthening a pattern from a knob is lossless and get_param reports
// what was written (#20).
class StepSequencer : public GateSequencer{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit StepSequencer(const NodeConfig& config);
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;
    protected:
        bool step_on(uint8_t step) const override {
            return (bits & ((uint32_t)1u << step)) != 0;
        }
    private:
        uint32_t bits;
};

// Bjorklund's even distribution of k pulses over n steps, plus a rotation.
// The pattern is computed once, when the node is constructed, and never per
// edge (see sequencer/euclid.h).
//
// params[0] steps (n)   params[3] pulses (k)   params[4] rotation
//
// All three re-derive the pattern at runtime (#20), so turning k from a
// controller is one Bjorklund run per *change* - not per message - and the
// step cursor is left where it is.
class EuclidianSequencer : public GateSequencer{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit EuclidianSequencer(const NodeConfig& config);
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t pulses() const { return k; }
        uint8_t rotation() const { return rot; }
    protected:
        bool step_on(uint8_t step) const override {
            return (bits & ((uint32_t)1u << step)) != 0;
        }
        void on_length_changed() override { derive(); }
    private:
        void derive();

        uint32_t bits;
        uint8_t k;
        uint8_t rot;
};

// Shred and load a random pattern.
//
// Seeded from the entropy pool rather than a constant, so a module does not
// play the same thing on every power cycle, and deterministic afterwards, so
// it plays the *same* thing until it is shredded. The shred is an inlet
// rather than only a config action: a pattern that another node can throw
// away mid-performance is the point of it.
//
// Inlet 2 (gate, optional): shred. A rising edge draws a new pattern.
// params[3] density, percent of steps on (0 -> 50)
// params[4] seed offset, so two identical configs do not agree
class RandomSequencer : public GateSequencer{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit RandomSequencer(const NodeConfig& config);
        // Draws a new pattern. Also reachable from the shred inlet.
        void shred();
        // Density and the seed offset move at runtime, but neither shreds:
        // a pattern that redrew itself every time a knob twitched would be
        // unplayable. The new density applies to the next shred, from the
        // inlet or from a host.
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;
    protected:
        bool step_on(uint8_t step) const override {
            return (bits & ((uint32_t)1u << step)) != 0;
        }
        void on_extra_edge() override { shred(); }
    private:
        uint32_t bits;
        uint8_t density;
        uint8_t seed_offset;
};

#endif
