#ifndef MMMC_ALGORITHM_SWITCH_GATE_SWITCH_H
#define MMMC_ALGORITHM_SWITCH_GATE_SWITCH_H

#include "node/node.h"
#include "algorithm/switch/selector.h"

// Many gates in, one out: the outlet carries whichever inlet is selected.
//
// This is the multiplexer, and with a note twin (NoteSwitch) it is how a
// patch has *parts*. Two drum patterns run all the time on two buses; the
// switch decides which one reaches the jack, and what decides the switch is
// the arrangement: a divider stepping it every four bars, a counter's count
// addressing it, a pad throwing its `select` from a CC. Nothing upstream
// knows it has been switched away from, which is the point - a part that is
// not playing keeps its place in the bar, so switching back lands in time.
//
// The one-to-many form is GateRouter. Selection itself - the parameter, the
// edges, the control inlet and the cycle - is Selector, shared by all four.
//
// Inlets 0..POSITIONS-1 (gate, optional): the parts.
// Inlet POSITIONS (CV, optional): the position, as a level.
// Inlet POSITIONS+1 (gate, optional): step.
// Inlet POSITIONS+2 (gate, optional): reset.
// Outlet 0 (gate): the selected inlet's level. A selected inlet with
//         nothing on it is low, which makes an empty position a rest.
//
// params[0] select  1..POSITIONS, the position; reads back as where it is
// params[1] steps   the cycle `step` wraps at (0 = up to the last patched)
class GateSwitch : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t POSITIONS = MAX_IN - 3;
        static constexpr uint8_t IN_SELECT = POSITIONS, IN_STEP = POSITIONS + 1, IN_RESET = POSITIONS + 2;
        static constexpr uint16_t P_SELECT = 0, P_STEPS = 1;

        explicit GateSwitch(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t position() const { return sel.position(); }

    private:
        BusSet in[POSITIONS];
        BusSet out;
        Selector sel;
};

// One gate in, many out: the inlet reaches whichever outlet is selected.
//
// The demultiplexer, and the other way of writing a song: rather than
// choosing which part is heard, choose which part is *clocked*. One
// metronome into the router and a sequencer on each outlet, and a part
// whose outlet is not selected simply does not advance - it holds its step
// and resumes from it, which a switch on the output cannot do. With a held
// gate on the inlet the router is a one-of-eight decoder: outlet k is high
// while position k is selected, which is a mute gate per section.
//
// Inlet 0 (gate, required): what is routed.
// Inlet 1 (CV, optional): the position, as a level.
// Inlet 2 (gate, optional): step.
// Inlet 3 (gate, optional): reset.
// Outlets 0..POSITIONS-1 (gate): the destinations.
//
// params[0] select  1..POSITIONS
// params[1] steps   the cycle `step` wraps at (0 = up to the last patched)
class GateRouter : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t POSITIONS = MAX_OUT;
        static constexpr uint8_t IN_SIGNAL = 0, IN_SELECT = 1, IN_STEP = 2, IN_RESET = 3;
        static constexpr uint16_t P_SELECT = 0, P_STEPS = 1;

        explicit GateRouter(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t position() const { return sel.position(); }

    private:
        BusSet in;
        BusSet out[POSITIONS];
        Selector sel;
};

#endif
