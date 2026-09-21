#include "algorithm/modulator/gate_to_cv.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::CV};

// `high` has a minimum of 1 so that a stored zero can mean its default of
// full scale; 0 % while up is `low` 100 and `high` 1, which is the same
// switch the other way round, one step short of the rail.
static const ParamDescriptor PARAMS[2] = {
    {"low",  0, 100, 0, PARAM_PERCENT, nullptr},
    {"high", 1, 100, GateToCv::DEFAULT_HIGH, PARAM_PERCENT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 2, PARAMS}};

static const char* const IN_NAMES[1] = {"gate"};
static const char* const OUT_NAMES[1] = {"level"};

const AlgorithmDescriptor GateToCv::descriptor = {
    ALGO_GATE_TO_CV, "GateToCV", 1, 1, 1, 2, IN, OUT, sizeof(GateToCv), false, construct_node<GateToCv>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A gate as a control level: one value while it is up, another while it is down.",
    CATEGORY_MODULATOR };

GateToCv::GateToCv(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    low(config.params[P_LOW] <= 100 ? config.params[P_LOW] : (uint8_t)0),
    high(config.params[P_HIGH] && config.params[P_HIGH] <= 100 ? config.params[P_HIGH] : DEFAULT_HIGH)
{}

void GateToCv::process(BusManager& bus, uint32_t){
    bus.cv_write(out, level_of(bus.gate_read(in) ? high : low));
}

bool GateToCv::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_LOW:  if (value > 100) return false; low = value; return true;
        case P_HIGH: if (value == 0 || value > 100) return false; high = value; return true;
        default: return false;
    }
}

uint8_t GateToCv::get_param(uint16_t index) const {
    switch (index){
        case P_LOW:  return low;
        case P_HIGH: return high;
        default: return 0;
    }
}
