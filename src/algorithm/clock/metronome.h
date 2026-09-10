#ifndef MMMC_ALGORITHM_CLOCK_METRONOME_H
#define MMMC_ALGORITHM_CLOCK_METRONOME_H

#include "node/node.h"
#include "clock/trigger_pulse.h"

// The master clock in note values (#4).
//
// ClockDiv is the exact instrument: an amount in PPQN ticks, a phase, a delay.
// It can express anything the subdivision can hold, and that is the problem -
// the amount is a count of ticks, so "one pulse per beat" is /24, a dotted
// eighth is /18, and a sixteenth-note triplet is /4, none of which reads as a
// note value unless you already know MASTER_PPQN is 24. Every one of them is a
// sum a musician has to do before hearing anything, and getting it wrong
// sounds like a tempo mistake rather than an arithmetic one.
//
// Metronome is the same clock said the way a musician says it: pick a note
// value, pick straight, dotted or triplet, and that is the rate. Nothing is
// lost - every division below is a whole number of subticks, so a Metronome
// is exactly as tight as the ClockDiv it replaces (see derive()) - and the
// divider is still there for the rates this list does not name.
//
// It was a GateSequencer of length one until now: an advance inlet whose
// every edge it passed through, plus a length, a direction, a reset and
// thirty-two per-step probabilities that a one-step pattern gives nothing to
// do. It sat behind a divider and did nothing the divider had not already
// done. The rate belongs *here*, where a metronome's rate is the only thing
// anybody wants to set.
//
// Inlet 0 (gate, optional): reset. A rising edge is a downbeat - the pulse
//         grid re-anchors on it and the next division is counted from there,
//         so a metronome can be re-phased by a jack, a logic gate or another
//         node's output without changing its rate.
// Outlet 0 (gate): a trigger of fixed width, like every other clock source in
//         the module - never a gate that stretches with tempo.
//
// params[0] division  a note value, "1/64" to "8 bars"; "1/4" is the beat
// params[1] feel      straight, dotted (x3/2) or triplet (x2/3)
// params[2] width     trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
//
// Live edits (#20): all three move at runtime. A new division is re-derived
// immediately but the pulse already scheduled is not moved, exactly as in
// ClockDiv - the current period completes rather than a pulse a musician is
// counting on being dragged out from under them.
class Metronome : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // Note values, slowest first, indexed from 1 so that a stored 0 can
        // still mean the descriptor's default (param.h). A "bar" is four
        // quarter notes: the module has no time signature, and 4/4 is the
        // only reading of "bar" that needs no other information.
        enum Division : uint8_t {
            DIV_8_BARS = 1,
            DIV_4_BARS = 2,
            DIV_2_BARS = 3,
            DIV_BAR    = 4,
            DIV_HALF   = 5,
            DIV_QUARTER = 6,
            DIV_EIGHTH = 7,
            DIV_16TH   = 8,
            DIV_32ND   = 9,
            DIV_64TH   = 10,
            DIVISIONS  = 10,
        };

        // What the note value is worth. Straight is the value itself, dotted
        // is half as long again, a triplet is three in the space of two.
        enum Feel : uint8_t {
            FEEL_STRAIGHT = 1,
            FEEL_DOTTED   = 2,
            FEEL_TRIPLET  = 3,
            FEELS         = 3,
        };

        explicit Metronome(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void tick(BusManager& bus, uint32_t count) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        uint8_t division() const { return div; }
        uint8_t feel() const { return how; }
        // Subticks between two pulses. Always a whole number - the assertions
        // in the .cpp are what keep it one.
        uint32_t period() const { return div_period; }
        uint32_t pulses() const { return pulse_count; }

    private:
        // Recomputes div_period from the division and the feel. Deliberately
        // leaves next_fire alone: see the class comment.
        void derive();
        void restart();
        void fire(BusManager& bus);
        // Advances the grid to `position` subticks, firing if a fire point
        // was passed. Whole periods are skipped rather than looped, so a node
        // loaded into a patch that has been running for an hour costs the
        // same as one loaded at zero.
        void advance_to(BusManager& bus, uint32_t position);

        uint8_t reset_in;
        uint8_t out;
        uint8_t div;             // Division
        uint8_t how;             // Feel
        uint8_t width_param;     // as stored, for get_param
        uint32_t div_period;     // subticks between two pulses
        uint32_t next_fire;
        uint32_t last_position;
        uint32_t pulse_count;
        uint32_t now;            // last timestamp process() saw
        TriggerPulse pulse;
        bool started;
        bool last_gate;
        bool pending_reset;      // a reset edge, applied on the next subtick
};

#endif
