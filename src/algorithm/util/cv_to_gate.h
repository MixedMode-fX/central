#ifndef MMMC_ALGORITHM_UTIL_CV_TO_GATE_H
#define MMMC_ALGORITHM_UTIL_CV_TO_GATE_H

#include "node/node.h"
#include "clock/trigger_pulse.h"

// A control signal becomes a gate: the comparator.
//
// The other half of CvToNote (algorithm/midi/cv_to_note.h), and it is written
// beside it deliberately. A patch that can turn a voltage into a note but not
// into the trigger that plays it is still stuck: the CV bus reaches the music
// through two doors, one for pitch and one for time, and neither is much use
// alone.
//
// What it is for, in order of how often it is wanted:
//
//   * **A clock that is not a metronome.** A slow Lfo through a comparator is
//     a pulse train whose spacing breathes - regular enough to be a pulse,
//     irregular enough not to be a grid. Two comparators at two thresholds on
//     one Lfo give two rhythms that are related rather than independent, which
//     is the difference between an ensemble and two sequencers.
//   * **A threshold on a modulator**, so something only happens at the top of
//     a sweep: a fill, a reset, an octave jump.
//   * **An envelope follower's gate**, once a CvInPort exists and an external
//     voltage is a bus like any other.
//
// **Hysteresis is not optional decoration.** A signal resting on the
// threshold crosses it on noise alone, and a comparator without hysteresis
// answers that with a burst of triggers - which on a gate bus is a stuck
// note or a machine-gunned drum. The output falls only once the signal has
// dropped `hysteresis` *below* where it rose, so a signal has to mean it.
//
// The level is read the way the modulation matrix reads one (see
// control/mod_matrix.cpp): bipolar adds half of full scale, so `threshold` at
// 50% is the zero crossing of a signal centred on zero, and unipolar clamps
// at zero. `invert` reverses the comparison rather than the wire, so the
// hysteresis stays on the side it belongs on and a `trigger` fires on the
// crossing the user asked for.
//
// Inlet 0 (CV): the signal.
// Outlet 0 (gate): the comparison.
//
// params[0] threshold   percent of full scale
// params[1] hysteresis  percent of full scale the signal must fall back
// params[2] polarity    how the level is read
// params[3] mode        gate (a level) / trigger (a fixed-width pulse)
// params[4] width       trigger width in ms (0 -> TRIGGER_WIDTH_US)
// params[5] invert      reverse the comparison
class CvToGate : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Polarity : uint8_t {
            CVG_BIPOLAR  = 1,
            CVG_UNIPOLAR = 2,
            CVG_POLARITIES = 2,
        };

        enum Mode : uint8_t {
            CVG_GATE    = 1,
            CVG_TRIGGER = 2,
            CVG_MODES   = 2,
        };

        explicit CvToGate(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests: whether the comparison is currently satisfied,
        // before `mode` decides what that puts on the bus.
        bool over() const { return high; }
        uint32_t crossings() const { return count; }

    private:
        uint8_t in;
        uint8_t out;
        uint8_t threshold;
        uint8_t hysteresis;
        uint8_t polarity;
        uint8_t mode;
        uint8_t width_param;
        uint8_t invert;
        bool high;               // the comparison, after hysteresis
        bool last_sense;         // the comparison as `invert` reports it
        uint32_t count;
        TriggerPulse pulse;
};

#endif
