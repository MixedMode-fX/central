#ifndef MMMC_ALGORITHM_MODULATOR_ENVELOPE_H
#define MMMC_ALGORITHM_MODULATOR_ENVELOPE_H

#include "node/node.h"
#include "clock/musical_division.h"
#include "clock/trigger_pulse.h"

// Envelope generators on a control bus: a contour a gate draws.
//
// The LFO is a shape that never stops and never listens. An envelope is the
// other modulator every machine needs - a shape with a *beginning*, fired by
// something that happened, which is what makes a modulation an articulation
// rather than a wobble. Through the modulation matrix (control/mod_matrix.h)
// the same contour is a filter sweep per note, a swell over eight bars, or a
// pitch dip on the attack of a drum; nothing here knows which, and one
// envelope reaches four parameters at four depths.
//
// **One mechanism, two nodes.** Every envelope on every module is the same
// walk through the same stages, and what actually differs is who ends it:
//
//   * `AD` is fired by an edge and runs to the end by itself. The gate's
//     length means nothing. That is a percussion envelope, and it is also
//     the function generator - set `loop` and it cycles.
//   * `ADSR` is *held*. It stops at a sustain level for as long as the gate
//     is up and falls when it is let go, so the contour is as long as the
//     note is.
//
// Everything else people name - AR, AHD, DADSR, an inverted pluck - is one of
// these two with a stage set to nothing or turned up, so it is a setting here
// rather than an algorithm id: an AR is an ADSR with `sustain` at 100 %, an
// AHD is an AD with a `hold`, and both have the pre-delay every stage list
// below starts with.
//
// **Stage times are wall-clock or note values, and the switch is one
// control.** `sync` picks which, exactly as it does on the LFO: free-running,
// each stage is a length in its own right (node/param.h's PARAM_ENV_TIME,
// half a millisecond to 32 seconds); locked to the clock, each stage is a
// note value from clock/musical_division.h at a shared `feel`, so an attack
// of "1/16" and a Metronome at "1/16" are the same length for ever and a
// looping envelope is in time with the pattern under it. A stage that should
// not be there at all is `off` when synced and a time of 1 - half a
// millisecond, one pass of the graph - when free.
//
// **A synced envelope is measured in the clock's own time.** Its stages
// advance on subticks, so it stretches and shrinks with the tempo, and a
// stopped clock freezes it where it stands rather than finishing it in
// wall-clock time behind the music's back.
//
// Outlet 1 is the **end of the contour**, as a trigger: what chains one
// envelope into the next, fires a sequencer a bar after a pad is let go, or
// turns a looping AD into a clock whose period is a shape.
class EnvelopeNode : public Node{
    public:
        // Where the contour is. Idle is not a stage the walk passes through:
        // it is the envelope at rest, output at zero, waiting to be fired.
        enum Stage : uint8_t {
            ENV_IDLE    = 0,
            ENV_DELAY   = 1,   // at rest, waiting: the pre-delay
            ENV_ATTACK  = 2,   // up to the peak
            ENV_HOLD    = 3,   // at the peak
            ENV_DECAY   = 4,   // down to the sustain level (to zero, on an AD)
            ENV_SUSTAIN = 5,   // there, while the gate is up (ADSR only)
            ENV_RELEASE = 6,   // down to zero (ADSR only)
            ENV_STAGES  = 7,
        };

        enum Sync : uint8_t {
            ENV_FREE  = 1,
            ENV_CLOCK = 2,
            ENV_SYNCS = 2,
        };

        // What happens at the end of the contour.
        enum Loop : uint8_t {
            ENV_LOOP_OFF  = 1,   // "off": it stops, and waits to be fired again
            ENV_LOOP_HELD = 2,   // "held": it starts over while the gate is still up
            ENV_LOOP_FREE = 3,   // "cycle": it starts over for ever, gate or no gate
            ENV_LOOPS     = 3,
        };

        // What a second edge does to a contour already running.
        enum Retrigger : uint8_t {
            ENV_RETRIG_ZERO   = 1,   // "zero": starts again from nothing, a hard retrigger
            ENV_RETRIG_LEVEL  = 2,   // "level": starts again from where it had got to
            ENV_RETRIG_IGNORE = 3,   // "ignore": nothing, until the contour has finished
            ENV_RETRIGGERS    = 3,
        };

        // Parameters 0 .. 11 are **the same two controls per stage in both
        // envelopes**, so the base class owns them and each node adds its own
        // tail from ENV_P_TAIL on. A stage that moves has a third control, the
        // curve it moves along; a stage that only waits does not.
        static constexpr uint16_t
            P_SYNC = 0, P_FEEL = 1,
            P_DELAY = 2,  P_DELAY_DIV = 3,
            P_ATTACK = 4, P_ATTACK_DIV = 5, P_ATTACK_CURVE = 6,
            P_HOLD = 7,   P_HOLD_DIV = 8,
            P_DECAY = 9,  P_DECAY_DIV = 10, P_DECAY_CURVE = 11,
            P_TAIL = 12;

        // Defaults, named because both descriptors and the constructor quote
        // them. A 32 ms attack and a 450 ms decay is a plucked note, which is
        // the first thing anybody patches an envelope into.
        static constexpr uint8_t DEFAULT_ATTACK = 8;     // 32 ms
        static constexpr uint8_t DEFAULT_DECAY = 30;     // 450 ms
        static constexpr uint8_t DEFAULT_RELEASE = 30;   // 450 ms
        static constexpr uint8_t DEFAULT_SUSTAIN = 60;   // percent
        static constexpr uint8_t DEFAULT_LEVEL = 100;    // percent
        static constexpr uint8_t OFF_TIME = 1;           // half a millisecond

        // The run of four both nodes end with, as offsets within that run.
        static constexpr uint16_t O_LEVEL = 0, O_INVERT = 1, O_LOOP = 2, O_RETRIG = 3, O_COUNT = 4;

        void process(BusManager& bus, uint32_t now_us) override;
        void tick(BusManager& bus, uint32_t count) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        Stage stage() const { return (Stage)at; }
        // The contour itself, 0 .. CV_MAX, before level and invert.
        uint16_t contour() const { return level; }
        // What was last written to the bus.
        int16_t value() const { return last_value; }
        // How long the current stage is, in whichever unit is running.
        uint32_t stage_length() const { return length; }

    protected:
        // `output_first` is where this node's `level` sits: the last four
        // parameters are the same run in both, at different indices, so the
        // base reads them and neither node writes them out twice.
        EnvelopeNode(const NodeConfig& config, bool sustaining, uint16_t output_first);

        // The tail each node adds. Index is already relative to P_TAIL.
        virtual bool set_tail(uint16_t index, uint8_t value) = 0;
        virtual uint8_t get_tail(uint16_t index) const = 0;

        // The four controls both tails end with, so neither node writes them
        // out twice. `which` is the parameter's offset within that run.
        bool set_output(uint16_t which, uint8_t value);
        uint8_t get_output(uint16_t which) const;

        // Recomputes the running stage's length and moves `elapsed` with it.
        // Called after any write that changes a length, because the write may
        // be to the stage that is running.
        void retime();

        // Stage lengths and curves, indexed by Stage. The entries a node does
        // not have (an AD's sustain and release) stay at their defaults and
        // are never read, because next_stage() never walks into them.
        uint8_t stage_time[ENV_STAGES];
        uint8_t stage_div[ENV_STAGES];
        uint8_t stage_curve[ENV_STAGES];
        uint8_t sustain;          // percent of the peak; 0 on an AD
        uint8_t level_pct;
        uint8_t invert;
        uint8_t loop;
        uint8_t retrig;
        uint8_t sync;
        uint8_t feel;

    private:
        // Back to the start of the contour, however `retrig` says to.
        void fire();
        // Stop, at zero.
        void idle();
        // Begin `next`, from wherever the level is now.
        void enter(uint8_t next);
        // The stage after this one, or ENV_IDLE when the contour is over.
        uint8_t next_stage(bool gate_high) const;
        // How long a stage is, in the unit the current `sync` counts in.
        // `retime()` is protected, above: the subclasses' parameters change
        // lengths too.
        uint32_t units_of(uint8_t which) const;
        // The contour's value part-way through the running stage.
        uint16_t level_at() const;
        // Where the running stage is heading.
        uint16_t target_of(uint8_t which) const;
        uint16_t sustain_level() const;

        BusSet gate_in;
        BusSet aux_in;
        BusSet out;
        BusSet eoc_out;

        TriggerPulse eoc;
        uint32_t elapsed;         // into the running stage, in the current unit
        uint32_t length;          // how long that stage is, same unit
        uint32_t clock_count;     // newest subtick count seen
        uint32_t taken_count;     // the subtick the running stage has counted to
        uint32_t last_us;
        uint16_t from_level;      // the contour where the running stage began
        uint16_t level;           // the contour now, 0 .. CV_MAX
        int16_t last_value;
        uint8_t at;               // Stage
        bool has_sustain;
        bool last_gate;
        bool last_aux;
        bool have_time;
        bool have_count;
};

// Attack and decay, fired by an edge: the percussion envelope, and the
// function generator.
//
// A trigger starts the contour and nothing stops it - the gate may be one
// pass long or a bar long and the shape is the same, which is what makes this
// the envelope for a drum, a pluck and anything else whose length is its own.
// `loop` turns it into a cycling shape generator: an AD set to `cycle` is an
// LFO whose rise and fall are set separately, and one set to `held` is a
// tremolo that plays only under a held note.
//
// Inlet 0 (gate, required): trig. A rising edge fires the contour.
// Inlet 1 (gate, optional): reset. A rising edge cuts it to zero at once -
//         how a looping envelope is stopped, and how a long fade is abandoned.
// Outlet 0 (CV): the contour.
// Outlet 1 (gate): a trigger at the end of every contour.
class AdEnvelope : public EnvelopeNode{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint16_t
            P_LEVEL = 12, P_INVERT = 13, P_LOOP = 14, P_RETRIG = 15, N_PARAMS = 16;

        explicit AdEnvelope(const NodeConfig& config) : EnvelopeNode(config, false, P_LEVEL) {}

    protected:
        bool set_tail(uint16_t index, uint8_t value) override { return set_output(index, value); }
        uint8_t get_tail(uint16_t index) const override { return get_output(index); }
};

// Delay, attack, hold, decay, sustain, release: the envelope that is as long
// as the note.
//
// It stops at `sustain` and stays there while the gate is up, so what comes
// out is the length of what was played - the difference between a stab and a
// swell being how long a pad was held rather than how a knob was set.
//
// **A looping ADSR skips the sustain**: with `loop` set to `held` the contour
// runs delay, attack, hold, decay and starts over for as long as the gate is
// down - a trill, a stutter, a ratchet whose rate is the stage times - and the
// release still happens when the gate is let go, from wherever in the loop it
// was. Set to `cycle`, it runs whether anything is played or not, and the gate
// no longer releases it.
//
// Inlet 0 (gate, required): gate. It rises to fire and falls to release.
// Inlet 1 (gate, optional): retrig. A rising edge re-fires the contour
//         without the gate having to fall first, which is what plays a second
//         note under a held first one.
// Outlet 0 (CV): the contour.
// Outlet 1 (gate): a trigger at the end of every contour.
class AdsrEnvelope : public EnvelopeNode{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint16_t
            P_SUSTAIN = 12,
            P_RELEASE = 13, P_RELEASE_DIV = 14, P_RELEASE_CURVE = 15,
            P_LEVEL = 16, P_INVERT = 17, P_LOOP = 18, P_RETRIG = 19, N_PARAMS = 20;

        explicit AdsrEnvelope(const NodeConfig& config);

    protected:
        bool set_tail(uint16_t index, uint8_t value) override;
        uint8_t get_tail(uint16_t index) const override;
};

// The curve a stage travels along. `amount` is a PARAM_CENTRED byte read as
// -100 .. +100 and `frac` is how far through the stage the contour is, 0 ..
// CV_FULL; the answer is how far through its *travel* it should be.
//
// Zero is a straight line. Below zero the stage moves fast and then slows,
// which on a falling stage is the exponential decay a struck string makes and
// on a rising one is the sharp attack of a plucked one; above zero it creeps
// and then runs, which is the swell a bow makes. One control, both senses,
// because a curve is a property of the travel and not of the direction.
uint32_t env_curve(uint8_t amount, uint32_t frac);

#endif
