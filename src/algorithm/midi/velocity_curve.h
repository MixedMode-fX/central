#ifndef MMMC_ALGORITHM_VELOCITY_CURVE_H
#define MMMC_ALGORITHM_VELOCITY_CURVE_H

#include "node/node.h"

// Reshapes note-on velocity. Pitch is untouched and no note is ever dropped,
// so this is the one modifier whose note-offs pair themselves: a note-off
// carries velocity 0 through unchanged and matches whatever note-on went out.
//
// The output is clamped to 1..127 rather than 0..127. A note-on with velocity
// 0 *is* a note-off, so a curve that reached zero would silently turn a note
// into its own release - which looks like a hanging voice that never sounded.
//
// params[0] curve   0 linear, 1 soft (easier to play loud), 2 hard, 3 fixed
// params[1] scale   percent, 0 -> 100
// params[2] offset  signed, applied after the curve and the scale
// params[3] fixed   the value curve 3 sends (0 -> 100)
class VelocityCurve : public Node{
    public:
        enum Curve : uint8_t { CURVE_LINEAR = 0, CURVE_SOFT = 1, CURVE_HARD = 2, CURVE_FIXED = 3 };

        static const AlgorithmDescriptor descriptor;
        explicit VelocityCurve(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t apply(uint8_t velocity) const;

    private:
        uint8_t in;
        uint8_t out;
        uint8_t curve;
        uint8_t scale;
        int8_t offset;
        uint8_t fixed;
};

#endif
