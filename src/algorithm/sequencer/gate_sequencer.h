#ifndef MMMC_ALGORITHM_SEQUENCER_GATE_SEQUENCER_H
#define MMMC_ALGORITHM_SEQUENCER_GATE_SEQUENCER_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "algorithm/sequencer/step_engine.h"

// The shape every gate sequencer shares (#6), on the step engine #13
// extracted from it.
//
// A sequencer has no clock of its own. Division is ClockDiv (#4), an ordinary
// node that writes a gate bus, so a sequencer is a gate-rate node with an
// **edge-triggered advance inlet** and any trigger source drives it: a
// divider, a logic gate, an external jack, or another sequencer's output.
// Several sequencers sharing one divider advance on the same edge, so they
// are locked together by construction rather than by being seeded alike.
//
// Inlet 0 (gate): advance. Each rising edge moves to the next step.
// Inlet 1 (gate, optional): reset. A rising edge returns the sequence to its
//         first step, which the next advance then plays. Reset works the same
//         way in every sequencer, and being driven by another node's output
//         is most of what makes the bus model worth having.
// Outlet 0 (gate): a trigger of fixed width when the step is on - never a
//         gate that stretches with tempo.
//
// params[0] length     1..MAX_SEQUENCE_LEN (0 -> the subclass's default)
// params[1] direction  StepEngine::Direction
// params[2] width      trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
// params[3..7]         the subclass's own
// params[8 + step]     probability of the step firing, percent (0 -> always)
class GateSequencer : public Node{
    public:
        static constexpr uint8_t PROBABILITY_BASE = 8;
        static constexpr uint8_t PARAM_COUNT = PROBABILITY_BASE + MAX_SEQUENCE_LEN;

        // The header block every gate sequencer shares. The four subclasses
        // differ only in params[3..7], so the descriptors below are built by
        // splicing a subclass block between this and the probability run.
        static const ParamDescriptor HEADER[3];
        static const ParamDescriptor PROBABILITY[1];
        // params[3..7] that a subclass does not use. Described rather than
        // left blank so an editor knows they are unused, and so the
        // descriptor table covers the whole parameter space.
        static const ParamDescriptor RESERVED[1];

        GateSequencer(const NodeConfig& config, uint8_t default_length);

        void process(BusManager& bus, uint32_t now_us) override;
        // params[0..2] and the params[8..] probability block. A subclass
        // handles its own params[3..7] and calls this for the rest; a length
        // change re-derives whatever the subclass caches, through
        // on_length_changed().
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t length() const { return engine.length(); }
        uint8_t position() const { return engine.position(); }
        uint32_t steps_taken() const { return engine.steps_taken(); }
        uint8_t probability(uint8_t step) const { return step < MAX_SEQUENCE_LEN ? chance[step] : 100; }
        // The pattern as a bitfield, step 0 in bit 0. For tests, the
        // emulator and #11.
        uint32_t pattern() const;
        // Whether the step is on, before probability is rolled.
        bool on(uint8_t step) const { return step < length() && step_on(step); }

    protected:
        // Whether the step fires. The only thing the four sequencers differ in.
        virtual bool step_on(uint8_t step) const = 0;
        // A rising edge on the reset inlet, for subclass state.
        virtual void on_reset() {}
        // A rising edge on a subclass's own third inlet, if it has one.
        virtual void on_extra_edge(){}
        // The length moved at runtime: a subclass that derives its pattern
        // from the length (Euclid) recomputes it here.
        virtual void on_length_changed(){}

        uint8_t default_len() const { return fallback; }

        StepEngine engine;
        Xorshift32 rng;

    private:
        EdgeIn advance_in;
        EdgeIn reset_in;
        EdgeIn extra_in;
        uint8_t out;
        uint8_t fallback;                    // the subclass's default length
        uint8_t width_param;                 // as stored, for get_param
        TriggerPulse pulse;
        uint8_t chance[MAX_SEQUENCE_LEN];
};

#endif
