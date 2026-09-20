#include "algorithm/clock/transport.h"
#include "node/registry.h"

static const Domain OUT[Transport::OUTLETS] = {Domain::Gate, Domain::Gate, Domain::Gate};

static const ParamDescriptor PARAMS[1] = {
    {"width", 0, 255, 0, PARAM_MILLIS, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 1, PARAMS}};

static const char* const OUT_NAMES[Transport::OUTLETS] = {"start", "stop", "continue"};

// Which transport edge each outlet answers to. The one place the two orders
// meet, so adding a fourth of either is a change in one table.
static const uint8_t EDGE_OF[Transport::OUTLETS] = {
    TRANSPORT_START, TRANSPORT_STOP, TRANSPORT_CONTINUE,
};

// No inlets at all: what this node reads is not on a bus. n_in is 0, so the
// domain and name tables are nullptr rather than empty - every reader of them
// is already bounded by n_in.
const AlgorithmDescriptor Transport::descriptor = {
    ALGO_TRANSPORT, "Transport", 0, 0, Transport::OUTLETS, 1, nullptr, OUT,
    sizeof(Transport), false, construct_node<Transport>,
    GROUPS, 1, nullptr, OUT_NAMES,
    "MIDI start, stop and continue as triggers: the transport resets the patch it drives.",
    CATEGORY_CLOCK };

Transport::Transport(const NodeConfig& config) :
    out(), width_param(config.params[0]), now(0), counts(), pulse()
{
    for (uint8_t w = 0; w < OUTLETS; w++){
        out[w] = config.out_buses[w];
        if (width_param) pulse[w].set_width_us((uint32_t)width_param * 1000u);
    }
}

bool Transport::set_param(uint16_t index, uint8_t value){
    if (index != 0) return false;
    width_param = value;
    const uint32_t us = value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US;
    for (uint8_t w = 0; w < OUTLETS; w++) pulse[w].set_width_us(us);
    return true;
}

uint8_t Transport::get_param(uint16_t index) const {
    return index == 0 ? width_param : (uint8_t)0;
}

void Transport::fire(BusManager& bus, uint8_t which){
    // The timestamp is the one process() saw this pass: transport_event()
    // runs immediately after it, in the same pass (master.cpp), so the pulse
    // is no older than the pass it was fired in.
    pulse[which].fire(now);
    counts[which]++;
    bus.gate_write(out[which], true);          // an unpatched outlet writes nowhere
}

void Transport::transport_event(BusManager& bus, uint8_t edges){
    for (uint8_t w = 0; w < OUTLETS; w++){
        if (edges & EDGE_OF[w]) fire(bus, w);
    }
}

void Transport::process(BusManager& bus, uint32_t now_us){
    now = now_us;
    // Hold each trigger up for its full width. A gate bus is cleared by the
    // swap, so a pulse that spans several passes is written on every one of
    // them - the level is the node's state, not the bus's.
    for (uint8_t w = 0; w < OUTLETS; w++){
        if (pulse[w].level(now_us)) bus.gate_write(out[w], true);
    }
}
