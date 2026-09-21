#include "algorithm/switch/cv_switch.h"
#include "node/registry.h"

// --- CvSwitch ----------------------------------------------------------------

static const Domain SWITCH_IN[MAX_IN] = {
    Domain::CV, Domain::CV, Domain::CV, Domain::CV, Domain::CV,
    Domain::CV, Domain::Gate, Domain::Gate };
static_assert(CvSwitch::POSITIONS == 5, "SWITCH_IN lists five signals, then select, step and reset");
static const Domain SWITCH_OUT[1] = {Domain::CV};

static const char* const SWITCH_IN_NAMES[MAX_IN] = {
    "in 1", "in 2", "in 3", "in 4", "in 5", "select", "step", "reset"};
static const char* const SWITCH_OUT_NAMES[1] = {"out"};

static const ParamDescriptor SWITCH_PARAMS[2] = {
    {"select", 1, CvSwitch::POSITIONS, 1, PARAM_NUMBER, nullptr},
    {"steps",  0, CvSwitch::POSITIONS, 0, PARAM_ENUM,   SWITCH_STEPS_NAMES},
};
static const ParamGroup SWITCH_GROUPS[1] = {{0, 1, 2, SWITCH_PARAMS}};

const AlgorithmDescriptor CvSwitch::descriptor = {
    ALGO_CV_SWITCH, "CvSwitch", MAX_IN, 0, 1, 2, SWITCH_IN, SWITCH_OUT,
    sizeof(CvSwitch), false, construct_node<CvSwitch>,
    SWITCH_GROUPS, 1, SWITCH_IN_NAMES, SWITCH_OUT_NAMES,
    "Many control signals to one: the outlet carries the selected inlet. Stepped, addressed or set.",
    CATEGORY_MODULATOR };

CvSwitch::CvSwitch(const NodeConfig& config) :
    in(),
    out(config.out_buses[0]),
    sel(config.in_buses[IN_SELECT], config.in_buses[IN_STEP], config.in_buses[IN_RESET],
        POSITIONS, ports_patched(config.in_buses, POSITIONS),
        config.params[P_SELECT], config.params[P_STEPS])
{
    for (uint8_t i = 0; i < POSITIONS; i++) in[i] = config.in_buses[i];
}

void CvSwitch::process(BusManager& bus, uint32_t){
    sel.update(bus);
    const BusSet chosen = in[sel.position()];
    bus.cv_write(out, chosen.any() ? bus.cv_read(chosen) : (int16_t)0);
}

bool CvSwitch::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_SELECT: return sel.set_select(value);
        case P_STEPS:  return sel.set_steps(value);
        default: return false;
    }
}

uint8_t CvSwitch::get_param(uint16_t index) const {
    switch (index){
        case P_SELECT: return sel.select();
        case P_STEPS:  return sel.steps();
        default: return 0;
    }
}

// --- CvRouter ----------------------------------------------------------------

static const Domain ROUTER_IN[4] = {Domain::CV, Domain::CV, Domain::Gate, Domain::Gate};
static const Domain ROUTER_OUT[MAX_OUT] = {
    Domain::CV, Domain::CV, Domain::CV, Domain::CV,
    Domain::CV, Domain::CV, Domain::CV, Domain::CV };
static_assert(CvRouter::POSITIONS == 8, "ROUTER_OUT lists one domain per outlet");

static const char* const ROUTER_IN_NAMES[4] = {"in", "select", "step", "reset"};
static const char* const ROUTER_OUT_NAMES[MAX_OUT] = {
    "out 1", "out 2", "out 3", "out 4", "out 5", "out 6", "out 7", "out 8"};

static const ParamDescriptor ROUTER_PARAMS[2] = {
    {"select", 1, CvRouter::POSITIONS, 1, PARAM_NUMBER, nullptr},
    {"steps",  0, CvRouter::POSITIONS, 0, PARAM_ENUM,   SWITCH_STEPS_NAMES},
};
static const ParamGroup ROUTER_GROUPS[1] = {{0, 1, 2, ROUTER_PARAMS}};

const AlgorithmDescriptor CvRouter::descriptor = {
    ALGO_CV_ROUTER, "CvRouter", 4, 1, MAX_OUT, 2, ROUTER_IN, ROUTER_OUT,
    sizeof(CvRouter), false, construct_node<CvRouter>,
    ROUTER_GROUPS, 1, ROUTER_IN_NAMES, ROUTER_OUT_NAMES,
    "One control signal to many: the inlet reaches the selected outlet; the rest read as zero.",
    CATEGORY_MODULATOR };

CvRouter::CvRouter(const NodeConfig& config) :
    in(config.in_buses[IN_SIGNAL]),
    out(),
    sel(config.in_buses[IN_SELECT], config.in_buses[IN_STEP], config.in_buses[IN_RESET],
        POSITIONS, ports_patched(config.out_buses, POSITIONS),
        config.params[P_SELECT], config.params[P_STEPS])
{
    for (uint8_t i = 0; i < POSITIONS; i++) out[i] = config.out_buses[i];
}

void CvRouter::process(BusManager& bus, uint32_t){
    sel.update(bus);
    bus.cv_write(out[sel.position()], bus.cv_read(in));
}

bool CvRouter::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_SELECT: return sel.set_select(value);
        case P_STEPS:  return sel.set_steps(value);
        default: return false;
    }
}

uint8_t CvRouter::get_param(uint16_t index) const {
    switch (index){
        case P_SELECT: return sel.select();
        case P_STEPS:  return sel.steps();
        default: return 0;
    }
}
