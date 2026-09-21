#ifndef MMMC_ALGORITHM_SWITCH_CV_SWITCH_H
#define MMMC_ALGORITHM_SWITCH_CV_SWITCH_H

#include "node/node.h"
#include "algorithm/switch/selector.h"

// Many control signals in, one out: the outlet carries the selected inlet.
//
// GateSwitch for the control bus. Three modulators run all the time - an
// LFO, a sample-and-hold, a staircase - and the switch says which one the
// modulation matrix reads this bar, so one route carries a different shape
// every phrase without a route being rewritten. Selection is Selector's:
// the parameter, a step, a reset, or a level on `select`, which makes a
// switch addressed by a Counter's count a sequencer of modulators.
//
// A selected inlet with nothing on it is zero, which is a modulation that
// rests. The unselected inlets are read by nobody and cost nothing.
//
// Inlets 0..POSITIONS-1 (CV, optional): the signals.
// Inlet POSITIONS (CV, optional): the position, as a level.
// Inlet POSITIONS+1 (gate, optional): step.
// Inlet POSITIONS+2 (gate, optional): reset.
// Outlet 0 (CV): the selected inlet's level, every pass.
//
// params[0] select  1..POSITIONS, the position; reads back as where it is
// params[1] steps   the cycle `step` wraps at (0 = up to the last patched)
class CvSwitch : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t POSITIONS = MAX_IN - 3;
        static constexpr uint8_t IN_SELECT = POSITIONS, IN_STEP = POSITIONS + 1, IN_RESET = POSITIONS + 2;
        static constexpr uint16_t P_SELECT = 0, P_STEPS = 1;

        explicit CvSwitch(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t position() const { return sel.position(); }

    private:
        BusSet in[POSITIONS];
        BusSet out;
        Selector sel;
};

// One control signal in, many out: the inlet reaches the selected outlet.
//
// GateRouter for the control bus, and the other half of a modulation
// patchbay: one LFO, and the router says which parameter's route it is on
// this bar. An outlet not selected is written by nobody, so the bus it is on
// empties and the parameter at the end of that route falls back to its own
// setting - which is what an unpatched modulation is, and the reason the
// router does not hold a last value on the outlets it has left.
//
// Inlet 0 (CV, required): what is routed.
// Inlet 1 (CV, optional): the position, as a level.
// Inlet 2 (gate, optional): step.
// Inlet 3 (gate, optional): reset.
// Outlets 0..POSITIONS-1 (CV): the destinations.
//
// params[0] select  1..POSITIONS
// params[1] steps   the cycle `step` wraps at (0 = up to the last patched)
class CvRouter : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t POSITIONS = MAX_OUT;
        static constexpr uint8_t IN_SIGNAL = 0, IN_SELECT = 1, IN_STEP = 2, IN_RESET = 3;
        static constexpr uint16_t P_SELECT = 0, P_STEPS = 1;

        explicit CvRouter(const NodeConfig& config);
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
