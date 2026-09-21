#ifndef MMMC_ALGORITHM_LOGIC_EDGE_H
#define MMMC_ALGORITHM_LOGIC_EDGE_H

#include "node/node.h"
#include "clock/trigger_pulse.h"

// Turns the edges of a gate into triggers: one outlet fires when the gate
// goes up, the other when it comes down.
//
// Every advance and reset inlet in the module is edge-triggered already, so
// a rising edge needs no help to clock something. What it cannot do on its
// own is be a trigger somewhere a level is wrong: a jack into a drum module
// that wants 5 ms and not a bar of gate, a GateToNote that should play a
// short note off a long hold. And the falling edge had no name at all - the
// end of a note, the release of a pad, the moment a held gate lets go. A NOT
// in front of an advance inlet clocks on it; this puts it on a jack.
//
// Inlet 0 (gate, required): the gate.
// Outlet 0 (gate): rise.
// Outlet 1 (gate): fall.
//
// Both outlets on one bus is "either edge", the gate bus being the OR of
// its writers.
//
// params[0] width  trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
class Edge : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t OUT_RISE = 0, OUT_FALL = 1;
        static constexpr uint16_t P_WIDTH = 0;

        explicit Edge(const NodeConfig& config);
        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

    private:
        BusSet in;
        BusSet rise_out;
        BusSet fall_out;
        TriggerPulse rise;
        TriggerPulse fall;
        uint8_t width_param;
        bool last;
        bool primed;        // the first pass sets `last` and fires nothing
};

#endif
