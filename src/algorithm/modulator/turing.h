#ifndef MMMC_ALGORITHM_MODULATOR_TURING_H
#define MMMC_ALGORITHM_MODULATOR_TURING_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"

// A looping shift register: the axis between a locked loop and pure noise.
//
// Every random source this module had was all or nothing. RandomSequencer
// shreds a whole new pattern, SampleHold draws a fresh level on every
// trigger, Probability flips a memoryless coin per step. None of them
// *remembers*, and the territory generative music actually lives in is the
// middle: a loop that repeats for eight bars and then changes one step.
//
// So: `length` bits in a ring. On each advance the top bit is fed back into
// the bottom, and on the way it is inverted with probability `chaos`. That
// one control has three landmarks and everything between them is useful:
//
//     chaos 0     nothing is ever inverted. The loop repeats for ever.
//     chaos 50    every bit is a coin flip. Nothing ever repeats.
//     chaos 100   every bit is inverted, every time. The loop still repeats,
//                 but it takes two passes to come back to itself, so a
//                 length of 8 is a 16-step pattern whose second half is the
//                 photographic negative of its first.
//
// Between 0 and 50 the loop survives and mutates one step at a time, which is
// the setting the node exists for; between 50 and 100 it does the same thing
// around the inverted loop.
//
// **Two outlets from one register, and that is the point.** The pulse is the
// bit that has just come round; the CV is the top `bits` of the register read
// as a number. Driving a rhythm from the first and a melody (through
// CvToNote) from the second means the two mutate *together* - the bar where
// the rhythm changes is the bar where the melody changes, because it is the
// same bit that moved. Two independent random sources sound like two random
// processes; one register sounds like a part.
//
// **Reset returns to the trunk.** The pattern the node started with is
// redrawn, so a wander that has gone somewhere unmusical is one edge away
// from the shape it grew out of. `seed` is what makes that reproducible: at
// zero the register is drawn from the entropy pool, so a module does not play
// the same thing on every power cycle, and at anything else it is drawn from
// that byte, so a preset plays the pattern it was saved with. This is the
// rule RandomSequencer already follows, for the same two reasons.
//
// `write` is the hand on the register: `clear` feeds zeros in and empties the
// loop a step at a time, `fill` feeds ones in and fills it. Neither is a
// reset - the loop is being rewritten while it runs, which is what makes
// clearing three steps out of eight a thing you can do on purpose.
//
// Inlet 0 (gate): advance.
// Inlet 1 (gate, optional): reset - redraw the pattern from `seed`.
// Outlet 0 (gate): a trigger when the bit that came round is set.
// Outlet 1 (CV): the register as a level.
//
// params[0] length     bits in the ring, 2..MAX_SEQUENCE_LEN
// params[1] chaos      percent chance of inverting the bit fed back
// params[2] bits       how many bits the CV reads: 1 is two levels, 8 is smooth
// params[3] write      follow / clear / fill
// params[4] seed       0 draws from the entropy pool, anything else is exact
// params[5] width      trigger width in ms (0 -> TRIGGER_WIDTH_US)
// params[6] polarity   how the CV outlet is centred
class Turing : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Write : uint8_t {
            TUR_FOLLOW = 1,   // the bit that came round, mutated by `chaos`
            TUR_CLEAR  = 2,   // feed zeros: the loop empties a step at a time
            TUR_FILL   = 3,   // feed ones
            TUR_WRITES = 3,
        };

        enum Polarity : uint8_t {
            TUR_UNIPOLAR = 1,   // 0 .. CV_MAX: a register value is a magnitude
            TUR_BIPOLAR  = 2,
            TUR_POLARITIES = 2,
        };

        static constexpr uint8_t MIN_LENGTH = 2;
        static constexpr uint8_t MAX_BITS = 8;

        explicit Turing(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        uint32_t reg() const { return shift; }
        // The bit that last came round.
        bool current() const { return (shift & 1u) != 0; }
        int16_t value() const { return level; }
        uint32_t steps() const { return count; }
        // The pattern as it stands, so a test can watch a loop survive.
        uint32_t pattern() const { return shift & mask(); }

    private:
        uint32_t mask() const {
            return len >= 32 ? 0xFFFFFFFFu : (uint32_t)((1u << len) - 1u);
        }
        // Redraws the register from `seed`, as construction and reset do.
        void draw();
        // One clock: rotate, mutating the bit that comes round.
        void shift_once();
        // Recomputes the CV level from the register.
        void derive();

        EdgeIn advance_in;
        EdgeIn reset_in;
        uint8_t gate_out;
        uint8_t cv_out;
        uint8_t len;
        uint8_t chaos;
        uint8_t bits;
        uint8_t write;
        uint8_t seed;
        uint8_t width_param;
        uint8_t polarity;
        uint32_t shift;
        uint32_t count;
        int16_t level;
        Xorshift32 rng;
        TriggerPulse pulse;
};

#endif
