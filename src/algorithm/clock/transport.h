#ifndef MMMC_ALGORITHM_CLOCK_TRANSPORT_H
#define MMMC_ALGORITHM_CLOCK_TRANSPORT_H

#include "node/node.h"
#include "clock/trigger_pulse.h"

// The transport as triggers: start, stop and continue on three gate outlets.
//
// MIDI start, stop and continue are transport-level. They reach the master
// clock and never a bus (README, "MIDI"), which is right - a realtime byte is
// not note traffic - and it leaves the rest of the patch unable to hear them.
// The clock itself does not need telling twice: a start puts the count back
// to subtick zero, so every node that counts *subticks* is on the downbeat
// with it. A node that counts *edges* is not. A sequencer's step, a
// GateHold's latch, a Turing register, an Automaton's rows: none of those is
// a function of the count, so after a stopped DAW they resume wherever they
// were left, and the patch plays the right notes in the wrong order.
//
// This node is the door from the transport into the gate domain, and closing
// that gap is the whole job. A trigger is what every reset inlet in the
// module takes, so `start` into the sequencers' resets means pressing play
// puts the patch back on step one, every time, with no cable from the DAW
// beyond the one already carrying the clock.
//
// **The edges are the transport's, not one port's.** Whatever moves the
// transport moves this: a realtime message on any MIDI input, `start` on the
// console, a CC bound to CC_TARGET_TRANSPORT, the app's transport buttons.
// A patch that resyncs from the transport therefore behaves the same on a
// bench with nothing plugged into it as it does in front of a DAW.
//
// Outlet 0 (gate): start.     Play from the top.
// Outlet 1 (gate): stop.
// Outlet 2 (gate): continue.  Resume where the transport stopped.
//
// All three are optional, and each is a fixed-width trigger like every other
// clock source in the module - never a gate that stretches with tempo.
//
// **A gate bus is the OR of its writers**, so pointing two of these at one
// bus is "either of them", with no logic node in between: start and continue
// into one reset is "whenever the transport rolls", which is what a mute or
// an envelope wants, while a sequencer usually wants start alone - a continue
// that reset it would not be a continue. That is also why the three are
// separate outlets rather than one outlet and a parameter choosing the
// message: the patch does the combining, and it can combine them differently
// for two different readers.
//
// There is no "running" outlet. GateHold in `latch` mode, `start` on its set
// inlet and `stop` on its reset, is that gate already - and reset wins there,
// so the one pass where both arrive still ends low.
//
// Every message fires, including one the clock was already in the state of: a
// second stop is a message the module was sent, and a start under a running
// clock is a re-sync, which is exactly the moment a patch most wants to be
// put back on step one.
//
// params[0] width  trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
//
// Live edits (#20): the width moves at runtime and applies to the next
// trigger, and to one still up - a pulse is measured against the width when
// it is asked for its level, not when it was fired.
class Transport : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // Outlet order. Which TransportEdge each one answers to is spelled
        // out in the .cpp rather than derived: an outlet index is preset
        // format and a bit in a mask is not, and neither should move because
        // the other did.
        enum Which : uint8_t {
            OUT_START    = 0,
            OUT_STOP     = 1,
            OUT_CONTINUE = 2,
            OUTLETS      = 3,
        };

        explicit Transport(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void transport_event(BusManager& bus, uint8_t edges) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        uint32_t fired(uint8_t which) const { return which < OUTLETS ? counts[which] : 0; }

    private:
        void fire(BusManager& bus, uint8_t which);

        BusSet out[OUTLETS];
        uint8_t width_param;     // as stored, for get_param
        uint32_t now;            // last timestamp process() saw
        uint32_t counts[OUTLETS];
        TriggerPulse pulse[OUTLETS];
};

#endif
