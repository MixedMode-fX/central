#ifndef MMMC_ALGORITHM_SEQUENCER_GATE_SEQUENCER_H
#define MMMC_ALGORITHM_SEQUENCER_GATE_SEQUENCER_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "util/random.h"

// The shape every gate sequencer shares (#6).
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
//         way in all four sequencers, and being driven by another node's
//         output is most of what makes the bus model worth having.
// Outlet 0 (gate): a trigger of fixed width when the step is on - never a
//         gate that stretches with tempo.
//
// params[0] length     1..MAX_SEQUENCE_LEN (0 -> the subclass's default)
// params[1] direction  0 forward, 1 reverse, 2 ping-pong, 3 random
// params[2] width      trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
class GateSequencer : public Node{
    public:
        enum Direction : uint8_t {
            SEQ_FORWARD = 0, SEQ_REVERSE = 1, SEQ_PINGPONG = 2, SEQ_RANDOM = 3,
        };

        GateSequencer(const NodeConfig& config, uint8_t default_length);

        void process(BusManager& bus, uint32_t now_us) override;

        uint8_t length() const { return len; }
        uint8_t position() const { return cursor; }
        uint32_t steps_taken() const { return steps; }
        // The pattern as a bitfield, step 0 in bit 0. For tests and #11.
        uint32_t pattern() const;

    protected:
        // Whether the step fires. The only thing the four sequencers differ in.
        virtual bool step_on(uint8_t step) const = 0;
        // A rising edge on the reset inlet, for subclass state.
        virtual void on_reset() {}
        // A rising edge on a subclass's own third inlet, if it has one.
        virtual void on_extra_edge(){}

        uint8_t len;
        Xorshift32 rng;

    private:
        void reset_sequence();
        void step_forward();

        uint8_t advance_in;
        uint8_t reset_in;
        uint8_t extra_in;
        uint8_t out;
        uint8_t direction;
        uint8_t cursor;
        uint32_t steps;
        TriggerPulse pulse;
        bool at_first;
        bool descending;
        bool last_advance;
        bool last_reset;
        bool last_extra;
};

#endif
