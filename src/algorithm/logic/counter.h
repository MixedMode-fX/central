#ifndef MMMC_ALGORITHM_LOGIC_COUNTER_H
#define MMMC_ALGORITHM_LOGIC_COUNTER_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"

// Counts clock edges modulo a length, and says where it is three ways.
//
// A sequencer with no pattern: the same StepEngine every sequencer runs, so
// `direction` means what it does there and a counter at length four
// pingpong visits the counts a pingpong sequencer would. What it plays is
// the count itself, on three kinds of outlet, and each of them is a thing
// a patch could not get from a divider:
//
//   carry   a trigger on the edge that brings the count back to its first
//           step - the wrap. Clocked by bars at length four it fires every
//           four bars, which is a phrase, and a NoteSwitch's `step` on it is
//           a song. The first edge after a reset lands on the first step and
//           fires nothing, so a counter and a switch reset together do not
//           lose a part to the bar that started them. Length one fires on
//           every edge, and going backwards the first step is the last.
//   count   the count as a level, 0..CV_MAX spread over the length: the
//           first step is 0, the next CV_FULL / length, and so on. Into a
//           switch's `select` inlet it addresses one part per count, so
//           the counter's direction is the arrangement's order - forward
//           is A B C D, pingpong is A B C D C B, random is a shuffle. Into
//           the modulation matrix it is a staircase locked to the clock.
//   /2 ../32  the bits of the count, as levels. At a length that is a
//           power of two they are the divider chain every logic module
//           has - square waves at half, a quarter, an eighth of the clock,
//           50 % duty, which a ClockDiv's triggers are not - and at any
//           other length they are still the count in binary, which is a
//           polyrhythm nobody would have written down.
//
// Inlet 0 (gate, required): clock.
// Inlet 1 (gate, optional): reset. The next edge counts from the first step.
// Outlet 0 (gate): carry.
// Outlet 1 (CV): count.
// Outlets 2..6 (gate): /2, /4, /8, /16, /32.
//
// params[0] length     1..MAX_SEQUENCE_LEN steps (0 -> DEFAULT_LENGTH)
// params[1] direction  StepEngine::Direction
// params[2] width      the carry trigger's width in ms (0 -> TRIGGER_WIDTH_US)
//
// Live edits (#20): a shorter length clamps on the next edge, as every
// sequencer does; a longer one carries on counting.
class Counter : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t DEFAULT_LENGTH = 4;
        static constexpr uint8_t BITS = 5;
        static constexpr uint8_t OUT_CARRY = 0, OUT_COUNT = 1, OUT_BIT = 2;
        static constexpr uint8_t OUTLETS = OUT_BIT + BITS;
        static constexpr uint16_t P_LENGTH = 0, P_DIRECTION = 1, P_WIDTH = 2;

        explicit Counter(const NodeConfig& config);
        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t position() const { return engine.position(); }
        uint8_t length() const { return engine.length(); }
        uint32_t carries() const { return carry_count; }

    private:
        EdgeIn clock_in;
        EdgeIn reset_in;
        BusSet carry_out;
        BusSet count_out;
        BusSet bit_out[BITS];
        StepEngine engine;
        Xorshift32 rng;
        TriggerPulse carry;
        uint8_t length_param;
        uint8_t width_param;
        uint32_t carry_count;
};

#endif
