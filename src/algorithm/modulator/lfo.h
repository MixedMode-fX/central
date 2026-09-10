#ifndef MMMC_ALGORITHM_MODULATOR_LFO_H
#define MMMC_ALGORITHM_MODULATOR_LFO_H

#include "node/node.h"
#include "clock/musical_division.h"
#include "util/random.h"

// A low-frequency oscillator on a CV bus - the module's first modulator.
//
// It writes a control value every pass, which the modulation matrix
// (control/mod_matrix.h) turns into parameter writes. Nothing about this node
// knows what it is modulating: it produces a signal, the matrix decides what
// the signal reaches, and the same LFO can drive four parameters at four
// depths without four LFOs.
//
// **Two rates, and only one of them is a guess.** Synced to the master clock
// the cycle is a note value from clock/musical_division.h, counted in
// subticks, so an LFO set to "1/4" and a Metronome set to "1/4" are locked
// together for ever and neither drifts. Free-running the cycle is wall-clock
// time in tenths of a hertz, which is what a modulator that should *not* line
// up with the music needs - a slow drift under a sequence is the whole point,
// and a synced one cannot express it.
//
// **Resolution.** The value is twelve bits (bus/domain.h), which is what
// makes this usable for fine control: a filter or a detune swept in 128 steps
// steps audibly, and this is 4096. The *phase* advances once per subtick when
// synced and once per pass when free - both around a millisecond, and neither
// is the limit on the value, because the value is a function of the phase and
// not a count of updates.
//
// Inlet 0 (gate, optional): reset. A rising edge restarts the cycle at
//         `phase`, so an LFO can be re-anchored by a jack or by a sequencer
//         without changing its rate.
// Outlet 0 (CV): the modulation signal.
//
// params[0] shape     sine / triangle / ramp up / ramp down / square /
//                     random step / random glide
// params[1] sync      free-running, or locked to the master clock
// params[2] rate      tenths of a hertz, free-running only: 0.1 Hz (a ten
//                     second cycle) to 25.5 Hz
// params[3] division  a note value, synced only
// params[4] feel      straight / dotted / triplet, synced only
// params[5] depth     0..255 as a fraction of full scale
// params[6] offset    signed, shifts the centre by up to half of full scale
// params[7] phase     0..255 as a fraction of a cycle: where a reset starts
// params[8] polarity  bipolar (centred on zero) or unipolar (0 .. full scale)
//
// Live edits (#20): all nine move at runtime. Rate and division re-derive the
// period immediately **without moving the phase** - an LFO whose rate is
// swept sweeps continuously instead of jumping back to the top of its cycle,
// which is the same rule ClockDiv follows for the pulse it has already
// scheduled.
class Lfo : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // Indexed from 1, so a stored 0 still means the descriptor's default.
        enum Shape : uint8_t {
            LFO_SINE     = 1,
            LFO_TRIANGLE = 2,
            LFO_RAMP_UP  = 3,
            LFO_RAMP_DOWN = 4,
            LFO_SQUARE   = 5,
            LFO_RANDOM_STEP = 6,   // a new random level every cycle
            LFO_RANDOM_GLIDE = 7,  // ... slid into over the cycle
            LFO_SHAPES   = 7,
        };

        enum Sync : uint8_t {
            LFO_FREE   = 1,
            LFO_CLOCK  = 2,
            LFO_SYNCS  = 2,
        };

        enum Polarity : uint8_t {
            LFO_BIPOLAR  = 1,
            LFO_UNIPOLAR = 2,
            LFO_POLARITIES = 2,
        };

        explicit Lfo(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void tick(BusManager& bus, uint32_t count) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        // Where in the cycle the oscillator is, 0 .. CV_MAX.
        uint16_t phase() const { return cycle_phase; }
        // The value last written to the bus.
        int16_t value() const { return last_value; }
        // Subticks in one cycle, when synced.
        uint32_t period_subticks() const { return sync_period; }

    private:
        // Recomputes sync_period from the division and the feel. Leaves the
        // phase alone: see the class comment.
        void derive();
        // The raw shape, 0 .. CV_MAX, for a phase 0 .. CV_MAX.
        uint16_t shape_at(uint16_t p) const;
        // Applies depth, offset and polarity to a raw shape value.
        int16_t scaled(uint16_t raw) const;
        // Draws the next random level and keeps the old one to glide from.
        void draw();
        // Back to the top of the cycle, in whichever mode is running.
        void restart();

        uint8_t reset_in;
        uint8_t out;
        uint8_t shape;
        uint8_t sync;
        uint8_t rate_param;      // tenths of a hertz
        uint8_t div;             // MusicalDivision
        uint8_t how;             // MusicalFeel
        uint8_t depth;
        uint8_t offset_param;    // as stored: 128..255 are -128..-1
        uint8_t start_phase;
        uint8_t polarity;

        uint32_t sync_period;    // subticks in one cycle
        uint32_t sync_origin;    // the subtick the current cycle started on
        uint32_t last_count;     // the newest subtick count seen
        uint32_t free_acc;       // free-running phase; the whole range is one cycle
        uint32_t free_inc;       // what one microsecond adds to it
        uint32_t last_us;        // the timestamp the last pass ran at
        uint16_t cycle_phase;    // 0 .. CV_MAX
        uint16_t random_from;    // the level a glide is coming from
        uint16_t random_to;      // the level this cycle holds or glides to
        int16_t last_value;
        Xorshift32 rng;
        bool started;
        bool last_gate;
        bool have_time;
};

#endif
