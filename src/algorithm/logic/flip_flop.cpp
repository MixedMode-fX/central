#include "algorithm/logic/flip_flop.h"
#include "node/registry.h"

static const Domain IN[4] = {Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Gate, Domain::Gate};

static const char* const TYPE_NAMES[FlipFlop::FF_TYPES] = {"D", "D latch", "T", "JK", "SR"};
static const char* const EDGE_NAMES[FlipFlop::EDGES] = {"rising", "falling"};

static const ParamDescriptor PARAMS[2] = {
    {"type", FlipFlop::FF_D, FlipFlop::FF_TYPES, FlipFlop::FF_D, PARAM_ENUM, TYPE_NAMES},
    {"edge", FlipFlop::EDGE_RISING, FlipFlop::EDGES, FlipFlop::EDGE_RISING, PARAM_ENUM, EDGE_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, 2, PARAMS}};

static const char* const IN_NAMES[4] = {"data", "K / R", "clock", "clear"};
static const char* const OUT_NAMES[2] = {"Q", "not Q"};

// min_in is 0: an SR with nothing patched is a bit a clear can drop, and
// every other type needs a clock that is still the user's to leave off.
const AlgorithmDescriptor FlipFlop::descriptor = {
    ALGO_FLIP_FLOP, "FlipFlop", 4, 0, 2, 2, IN, OUT, sizeof(FlipFlop), false, construct_node<FlipFlop>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "One clocked bit: D, transparent latch, toggle, JK or SR, with Q, not Q and a clear.",
    CATEGORY_LOGIC };

static uint8_t clamp_enum(uint8_t stored, uint8_t top, uint8_t fallback){
    return (stored == 0 || stored > top) ? fallback : stored;
}

FlipFlop::FlipFlop(const NodeConfig& config) :
    data_in(config.in_buses[IN_DATA]),
    k_in(config.in_buses[IN_K]),
    clock_in(config.in_buses[IN_CLOCK]),
    clear_in(config.in_buses[IN_CLEAR]),
    q_out(config.out_buses[0]),
    nq_out(config.out_buses[1]),
    type(clamp_enum(config.params[P_TYPE], FF_TYPES, FF_D)),
    edge(clamp_enum(config.params[P_EDGE], EDGES, EDGE_RISING)),
    bit(false), last_clock(false), last_clear(false)
{}

void FlipFlop::process(BusManager& bus, uint32_t){
    const bool d = data_in.any() && bus.gate_read(data_in);
    const bool k = k_in.any() && bus.gate_read(k_in);

    // The clock's edge and its open level, in the polarity `edge` names.
    const bool clock = clock_in.any() && bus.gate_read(clock_in);
    const bool active = (edge == EDGE_FALLING) ? !clock : clock;
    const bool last_active = (edge == EDGE_FALLING) ? !last_clock : last_clock;
    const bool clocked = clock_in.any() && active && !last_active;
    last_clock = clock;

    switch (type){
        case FF_D_LATCH:
            if (clock_in.any() && active) bit = d;
            break;
        case FF_T:
            if (clocked && (d || !data_in.any())) bit = !bit;
            break;
        case FF_JK:
            if (clocked){
                if (d && k) bit = !bit;
                else if (d) bit = true;
                else if (k) bit = false;
            }
            break;
        case FF_SR:
            if (!clock_in.any() || clocked){
                if (k) bit = false;
                else if (d) bit = true;
            }
            break;
        default:                                // FF_D
            if (clocked) bit = d;
            break;
    }

    if (clear_in.any()){
        const bool c = bus.gate_read(clear_in);
        if (c && !last_clear) bit = false;
        last_clear = c;
    }

    if (bit) bus.gate_write(q_out, true);
    else bus.gate_write(nq_out, true);
}

bool FlipFlop::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_TYPE:
            if (value == 0 || value > FF_TYPES) return false;
            type = value;                       // the bit is kept: see the header
            return true;
        case P_EDGE:
            if (value == 0 || value > EDGES) return false;
            edge = value;
            return true;
        default:
            return false;
    }
}

uint8_t FlipFlop::get_param(uint16_t index) const {
    switch (index){
        case P_TYPE: return type;
        case P_EDGE: return edge;
        default: return 0;
    }
}
