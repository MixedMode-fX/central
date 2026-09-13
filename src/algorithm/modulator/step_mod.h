#ifndef MMMC_ALGORITHM_MODULATOR_STEP_MOD_H
#define MMMC_ALGORITHM_MODULATOR_STEP_MOD_H

#include "node/node.h"
#include "algorithm/modulator/shape.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"

// A stepped modulator whose frequency is its trigger inlet.
//
// An Lfo has a shape and a rate and reads the shape continuously. This has
// the same shape (algorithm/modulator/shape.h) cut into `steps` levels, and
// no rate at all: each trigger moves to the next step and the level is held
// until the next one. What comes out is a stepped version of the curve, and
// how fast it runs is whatever is patched to `trigger` - a ClockDiv, a
// sequencer's output, a logic gate, an external jack. One period is `steps`
// triggers, so the modulation is locked to the rhythm by construction rather
// than by a rate set to match it.
//
// **This is not a SampleHold.** That one reads whatever is on a bus and is at
// the mercy of it; this one knows the shape it is drawing, so eight steps of
// a triangle are eight evenly spaced levels up and down, every period,
// exactly. Which is what makes it worth having a `direction`: the steps of a
// known shape can be walked backwards, bounced or shuffled, and a SampleHold
// has nothing to walk.
//
// **A step samples the centre of its slice of the shape, not the start.**
// Four steps of a ramp are 12.5%, 37.5%, 62.5% and 87.5% of full scale, not
// 0/25/50/75. Sampling the start would make the first step of a ramp the
// bottom rail and the last step fall short of the top, and would put a sine's
// peak between two steps rather than on one; centres are symmetric, so a
// triangle walked up and back down visits the same levels both ways.
//
// Inlet 0 (gate, required): trigger. A rising edge moves to the next step.
// Inlet 1 (gate, optional): reset. A rising edge returns the period to its
//         first step, which the next trigger then plays - the same thing
//         reset means in every sequencer (algorithm/sequencer/step_engine.h),
//         so the level does not move until a trigger does.
// Outlet 0 (CV): the held level.
//
// params[0] shape      sine / triangle / ramp up / ramp down / square /
//                      random step / random glide
// params[1] steps      how many steps one period of the shape is cut into
// params[2] direction  StepEngine::Direction: which step a trigger goes to
// params[3] depth      0..255 as a fraction of full scale
// params[4] offset     signed, shifts the centre by up to half of full scale
// params[5] polarity   bipolar (centred on zero) or unipolar (0 .. full scale)
//
// **The two random shapes draw on different beats**, because a stepped
// modulator is the one place where they mean different things: `random step`
// draws a fresh level on every trigger - the classic noise-per-step - while
// `random glide` draws one target per period and steps towards it from the
// last one, which is a stepped ramp between random points.
//
// Live edits (#20): all six move at runtime and every one of them re-reads
// the level of the step being held, so a control does something the moment it
// is moved rather than on the next trigger. A `steps` change is the one
// exception the step engine imposes: the cursor is clamped on the next
// trigger rather than immediately, because a jump reorders a running pattern.
class StepMod : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Polarity : uint8_t {
            STEP_BIPOLAR  = 1,
            STEP_UNIPOLAR = 2,
            STEP_POLARITIES = 2,
        };

        static const uint8_t DEFAULT_STEPS = 8;

        explicit StepMod(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        // The value last written to the bus.
        int16_t value() const { return held; }
        // The step being held, 0 .. steps-1.
        uint8_t step() const { return cursor; }
        uint8_t steps() const { return engine.length(); }
        uint32_t triggers() const { return engine.steps_taken(); }
        // Where in the shape that step reads, 0 .. CV_MAX.
        uint16_t phase() const { return phase_of(cursor); }

    private:
        // The centre of step `i`'s slice of the shape.
        uint16_t phase_of(uint8_t i) const;
        // Re-reads the shape at the current step and rescales it. Never
        // draws: the random levels move on triggers, not on parameter edits.
        void refresh();
        // Draws the next random level and keeps the old one to glide from.
        void draw();

        StepEngine engine;
        Xorshift32 rng;
        EdgeIn trigger_in;
        EdgeIn reset_in;
        uint8_t out;
        uint8_t shape;
        uint8_t depth;
        uint8_t offset_param;    // as stored: 128..255 are -128..-1
        uint8_t polarity;
        uint8_t cursor;          // the step being held
        uint16_t random_from;    // the level a glide is coming from
        uint16_t random_to;      // the level this period holds or glides to
        int16_t held;
};

#endif
