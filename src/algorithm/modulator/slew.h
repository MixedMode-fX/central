#ifndef MMMC_ALGORITHM_MODULATOR_SLEW_H
#define MMMC_ALGORITHM_MODULATOR_SLEW_H

#include "node/node.h"

// A slew limiter on a control bus: what a value is allowed to change by, per
// unit of time.
//
// Everything upstream of this produces steps. A sample and hold jumps to a
// new level on every trigger, a square LFO jumps twice a cycle, and a
// parameter driven straight from either lands on the new value in one pass.
// Sometimes that is the point; often it is the difference between a modulation
// and a click. This is the node that makes the same source glide instead, and
// it is the reason the modulation matrix does not need a smoothing control of
// its own - smoothing is a signal operation, so it belongs in the signal path
// where it can be shared, metered and patched around.
//
// Rise and fall are separate, because they usually want to be: a fast attack
// and a slow decay out of a sample and hold is an envelope, and one rate for
// both is not. `link` ties fall to rise for the case where they do.
//
// Times are **per full scale**, in tens of milliseconds: a rate of 50 is half
// a second to cross the whole range, and a change of a quarter of the range
// at that rate takes an eighth of a second. Expressing it as a time for a
// fixed distance rather than a time to arrive is what makes two different
// step sizes glide at the same speed instead of taking the same time.
//
// Inlet 0 (CV, required): the signal to follow.
// Outlet 0 (CV): the slewed signal.
//
// params[0] rise  tens of milliseconds per full scale upwards (0 = instant)
// params[1] fall  the same, downwards
// params[2] link  fall follows rise
class Slew : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        explicit Slew(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        int16_t value() const { return (int16_t)(level >> SUB_BITS); }

    private:
        // The output is carried at a finer resolution than the bus so that a
        // slow slew moves at all: at 2.55 seconds per full scale and a pass
        // every millisecond, one pass is 1.6 bus units, and truncating that
        // to a whole unit every pass would round a slow glide to a stop.
        // Eight sub-bits make the smallest step the node can take 1/256 of a
        // bus unit, which is far below the slowest rate it can be set to.
        static constexpr uint8_t SUB_BITS = 8;

        // How far the level may move in `dt` microseconds at `rate`, in
        // sub-units. Zero rate is instant, and is reported as the whole range.
        int32_t step_for(uint8_t rate, uint32_t dt) const;

        uint8_t in;
        uint8_t out;
        uint8_t rise;
        uint8_t fall;
        uint8_t link;
        int32_t level;           // the output, in sub-units
        uint32_t last_us;
        bool have_time;
};

#endif
