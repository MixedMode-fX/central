#include "algorithm/logic/edge.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[2] = {Domain::Gate, Domain::Gate};

static const ParamDescriptor PARAMS[1] = {
    {"width", 1, 255, TRIGGER_WIDTH_US / 1000, PARAM_MILLIS, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 1, PARAMS}};

static const char* const IN_NAMES[1] = {"gate"};
static const char* const OUT_NAMES[2] = {"rise", "fall"};

const AlgorithmDescriptor Edge::descriptor = {
    ALGO_EDGE, "Edge", 1, 1, 2, 1, IN, OUT, sizeof(Edge), false, construct_node<Edge>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A trigger when a gate goes up, and another when it comes down.",
    CATEGORY_LOGIC };

Edge::Edge(const NodeConfig& config) :
    in(config.in_buses[0]),
    rise_out(config.out_buses[OUT_RISE]),
    fall_out(config.out_buses[OUT_FALL]),
    rise(), fall(),
    width_param(config.params[P_WIDTH]),
    last(false), primed(false)
{
    if (width_param){
        rise.set_width_us((uint32_t)width_param * 1000u);
        fall.set_width_us((uint32_t)width_param * 1000u);
    }
}

void Edge::process(BusManager& bus, uint32_t now_us){
    const bool level = bus.gate_read(in);
    // A gate that is already up when the patch loads is not an edge: the
    // first pass only learns the level.
    if (primed){
        if (level && !last) rise.fire(now_us);
        if (!level && last) fall.fire(now_us);
    }
    last = level;
    primed = true;
    if (rise.level(now_us)) bus.gate_write(rise_out, true);
    if (fall.level(now_us)) bus.gate_write(fall_out, true);
}

bool Edge::set_param(uint16_t index, uint8_t value){
    if (index != P_WIDTH || value == 0) return false;
    width_param = value;
    rise.set_width_us((uint32_t)value * 1000u);
    fall.set_width_us((uint32_t)value * 1000u);
    return true;
}

uint8_t Edge::get_param(uint16_t index) const {
    if (index != P_WIDTH) return 0;
    return width_param ? width_param : (uint8_t)(TRIGGER_WIDTH_US / 1000);
}
