#ifndef MMMC_ALGORITHM_MODULATOR_SAMPLE_HOLD_H
#define MMMC_ALGORITHM_MODULATOR_SAMPLE_HOLD_H

#include "node/node.h"
#include "util/random.h"

// Sample and hold: take one reading of a control signal and keep it until
// the next trigger.
//
// The oldest modular utility there is, and the one that turns a clock into a
// modulation source. With nothing patched to `signal` it samples its own
// noise, which is the classic patch - a random level per step, held steady
// between steps - and with something patched it holds a reading of that
// instead, which is how a slow LFO becomes a stepped sequence locked to the
// clock rather than a smooth sweep.
//
// Two modes, and the difference is what happens between triggers:
//
//   sample  read on the rising edge and hold until the next one. The classic.
//   track   follow the input for as long as the trigger is up, and freeze on
//           the falling edge. A gate, rather than a trigger, decides how much
//           of the input gets through.
//
// `steps` quantises what is held. The bus is twelve bits (bus/domain.h) and
// that is the point of it - but a random level quantised to four or eight
// values is a different instrument from one that can be anything, and it is
// the same instrument a musician means by "a random note from the scale". At
// 0 or 1 nothing is quantised and the full twelve bits are held. The grid is
// centred on zero, so a bipolar signal is quantised as evenly as a unipolar
// one.
//
// Inlet 0 (gate, required): trigger.
// Inlet 1 (CV, optional): the signal to sample. Unconnected, the node samples
//         its own random source - which is what `source` says explicitly when
//         a user wants noise from a node that *is* connected.
// Outlet 0 (CV): the held level.
//
// params[0] source  follow the inlet, or always the internal random source
// params[1] mode    sample / track
// params[2] steps   quantise the held value to this many levels (0, 1 = off)
//
// The held value survives a parameter edit (#20) - changing `steps` requantises
// the level that is already held rather than waiting for the next trigger, so
// the control does something the moment it is moved.
class SampleHold : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Source : uint8_t {
            SH_AUTO   = 1,   // the inlet if it is connected, random if it is not
            SH_SIGNAL = 2,   // the inlet, even when it is unconnected (holds zero)
            SH_RANDOM = 3,   // the internal random source, even when connected
            SH_SOURCES = 3,
        };

        enum Mode : uint8_t {
            SH_SAMPLE = 1,
            SH_TRACK  = 2,
            SH_MODES  = 2,
        };

        explicit SampleHold(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        int16_t value() const { return held; }
        uint32_t samples() const { return sample_count; }

    private:
        // Rounds `v` onto `steps` evenly spaced levels across full scale.
        int16_t quantise(int16_t v) const;
        // One reading of whatever the source is.
        int16_t take(BusManager& bus);

        uint8_t trigger_in;
        uint8_t signal_in;
        uint8_t out;
        uint8_t source;
        uint8_t mode;
        uint8_t steps;
        int16_t held;
        int16_t raw;             // what was read, before quantising
        uint32_t sample_count;
        Xorshift32 rng;
        bool last_gate;
};

#endif
