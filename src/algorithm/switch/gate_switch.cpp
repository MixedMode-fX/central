#include "algorithm/switch/gate_switch.h"
#include "node/registry.h"

const char* const SWITCH_STEPS_NAMES[9] = {
    "patched", "1", "2", "3", "4", "5", "6", "7", "8",
};

// --- GateSwitch --------------------------------------------------------------

static const Domain SWITCH_IN[MAX_IN] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate,
    Domain::CV, Domain::Gate, Domain::Gate };
static_assert(GateSwitch::POSITIONS == 5, "SWITCH_IN lists five parts, then select, step and reset");
static const Domain SWITCH_OUT[1] = {Domain::Gate};

static const char* const SWITCH_IN_NAMES[MAX_IN] = {
    "in 1", "in 2", "in 3", "in 4", "in 5", "select", "step", "reset"};
static const char* const SWITCH_OUT_NAMES[1] = {"out"};

static const ParamDescriptor SWITCH_PARAMS[2] = {
    {"select", 1, GateSwitch::POSITIONS, 1, PARAM_NUMBER, nullptr},
    {"steps",  0, GateSwitch::POSITIONS, 0, PARAM_ENUM,   SWITCH_STEPS_NAMES},
};
static const ParamGroup SWITCH_GROUPS[1] = {{0, 1, 2, SWITCH_PARAMS}};

// min_in is 0: a switch with one part patched is a mute, and one with none
// is a position a CC can read back.
const AlgorithmDescriptor GateSwitch::descriptor = {
    ALGO_GATE_SWITCH, "GateSwitch", MAX_IN, 0, 1, 2, SWITCH_IN, SWITCH_OUT,
    sizeof(GateSwitch), false, construct_node<GateSwitch>,
    SWITCH_GROUPS, 1, SWITCH_IN_NAMES, SWITCH_OUT_NAMES,
    "Many gates to one: the outlet carries the selected inlet. Stepped, addressed or set.",
    CATEGORY_LOGIC };

GateSwitch::GateSwitch(const NodeConfig& config) :
    in(),
    out(config.out_buses[0]),
    sel(config.in_buses[IN_SELECT], config.in_buses[IN_STEP], config.in_buses[IN_RESET],
        POSITIONS, ports_patched(config.in_buses, POSITIONS),
        config.params[P_SELECT], config.params[P_STEPS])
{
    for (uint8_t i = 0; i < POSITIONS; i++) in[i] = config.in_buses[i];
}

void GateSwitch::process(BusManager& bus, uint32_t){
    sel.update(bus);
    const BusSet chosen = in[sel.position()];
    bus.gate_write(out, chosen.any() && bus.gate_read(chosen));
}

bool GateSwitch::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_SELECT: return sel.set_select(value);
        case P_STEPS:  return sel.set_steps(value);
        default: return false;
    }
}

uint8_t GateSwitch::get_param(uint16_t index) const {
    switch (index){
        case P_SELECT: return sel.select();
        case P_STEPS:  return sel.steps();
        default: return 0;
    }
}

// --- GateRouter --------------------------------------------------------------

static const Domain ROUTER_IN[4] = {Domain::Gate, Domain::CV, Domain::Gate, Domain::Gate};
static const Domain ROUTER_OUT[MAX_OUT] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate,
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };
static_assert(GateRouter::POSITIONS == 8, "ROUTER_OUT lists one domain per outlet");

static const char* const ROUTER_IN_NAMES[4] = {"in", "select", "step", "reset"};
static const char* const ROUTER_OUT_NAMES[MAX_OUT] = {
    "out 1", "out 2", "out 3", "out 4", "out 5", "out 6", "out 7", "out 8"};

static const ParamDescriptor ROUTER_PARAMS[2] = {
    {"select", 1, GateRouter::POSITIONS, 1, PARAM_NUMBER, nullptr},
    {"steps",  0, GateRouter::POSITIONS, 0, PARAM_ENUM,   SWITCH_STEPS_NAMES},
};
static const ParamGroup ROUTER_GROUPS[1] = {{0, 1, 2, ROUTER_PARAMS}};

const AlgorithmDescriptor GateRouter::descriptor = {
    ALGO_GATE_ROUTER, "GateRouter", 4, 1, MAX_OUT, 2, ROUTER_IN, ROUTER_OUT,
    sizeof(GateRouter), false, construct_node<GateRouter>,
    ROUTER_GROUPS, 1, ROUTER_IN_NAMES, ROUTER_OUT_NAMES,
    "One gate to many: the inlet reaches the selected outlet. A held gate in makes it a decoder.",
    CATEGORY_LOGIC };

GateRouter::GateRouter(const NodeConfig& config) :
    in(config.in_buses[IN_SIGNAL]),
    out(),
    sel(config.in_buses[IN_SELECT], config.in_buses[IN_STEP], config.in_buses[IN_RESET],
        POSITIONS, ports_patched(config.out_buses, POSITIONS),
        config.params[P_SELECT], config.params[P_STEPS])
{
    for (uint8_t i = 0; i < POSITIONS; i++) out[i] = config.out_buses[i];
}

void GateRouter::process(BusManager& bus, uint32_t){
    sel.update(bus);
    const bool level = bus.gate_read(in);
    if (level) bus.gate_write(out[sel.position()], true);
}

bool GateRouter::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_SELECT: return sel.set_select(value);
        case P_STEPS:  return sel.set_steps(value);
        default: return false;
    }
}

uint8_t GateRouter::get_param(uint16_t index) const {
    switch (index){
        case P_SELECT: return sel.select();
        case P_STEPS:  return sel.steps();
        default: return 0;
    }
}
