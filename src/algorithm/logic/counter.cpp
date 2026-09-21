#include "algorithm/logic/counter.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[Counter::OUTLETS] = {
    Domain::Gate, Domain::CV, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };
static_assert(Counter::OUTLETS == 7, "OUT lists carry, count and five bits");

static const ParamDescriptor PARAMS[3] = {
    {"length",    1, MAX_SEQUENCE_LEN, Counter::DEFAULT_LENGTH, PARAM_NUMBER, nullptr},
    {"direction", 0, StepEngine::SEQ_DIRECTIONS - 1, 0, PARAM_ENUM, PARAM_DIRECTION_NAMES},
    {"width",     1, 255, TRIGGER_WIDTH_US / 1000, PARAM_MILLIS, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 3, PARAMS}};

static const char* const IN_NAMES[2] = {"clock", "reset"};
static const char* const OUT_NAMES[Counter::OUTLETS] = {"carry", "count", "/2", "/4", "/8", "/16", "/32"};

const AlgorithmDescriptor Counter::descriptor = {
    ALGO_COUNTER, "Counter", 2, 1, Counter::OUTLETS, 3, IN, OUT, sizeof(Counter), false, construct_node<Counter>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Counts edges modulo a length: a carry on the wrap, the count as a level, its bits as squares.",
    CATEGORY_LOGIC };

Counter::Counter(const NodeConfig& config) :
    clock_in(config.in_buses[0]),
    reset_in(config.in_buses[1]),
    carry_out(config.out_buses[OUT_CARRY]),
    count_out(config.out_buses[OUT_COUNT]),
    bit_out(),
    engine(),
    rng(entropy::seed()),
    carry(),
    length_param(config.params[P_LENGTH]),
    width_param(config.params[P_WIDTH]),
    carry_count(0)
{
    for (uint8_t b = 0; b < BITS; b++) bit_out[b] = config.out_buses[OUT_BIT + b];
    engine.configure(length_param, config.params[P_DIRECTION], DEFAULT_LENGTH);
    if (width_param) carry.set_width_us((uint32_t)width_param * 1000u);
}

void Counter::process(BusManager& bus, uint32_t now_us){
    // Reset wins over a clock in the same pass: the edge that arrives with
    // the reset is the first step, not the step after it.
    if (reset_in.rising(bus)) engine.reset();
    if (clock_in.rising(bus)){
        const bool from_start = engine.before_first();
        const uint8_t was = engine.position();
        const uint8_t len = engine.length();
        const uint8_t now = engine.advance(rng);
        const uint8_t first = engine.direction() == StepEngine::SEQ_REVERSE ? (uint8_t)(len - 1u) : 0;
        if (!from_start && now == first && (was != first || len == 1)){
            carry.fire(now_us);
            carry_count++;
        }
    }

    if (carry.level(now_us)) bus.gate_write(carry_out, true);
    if (!engine.before_first()){
        const uint8_t pos = engine.position();
        bus.cv_write(count_out, (int16_t)(((int32_t)pos * CV_FULL) / engine.length()));
        for (uint8_t b = 0; b < BITS; b++){
            if ((pos >> b) & 1u) bus.gate_write(bit_out[b], true);
        }
    }
}

bool Counter::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_LENGTH:
            if (value == 0 || value > MAX_SEQUENCE_LEN) return false;
            length_param = value;
            engine.set_length(value, DEFAULT_LENGTH);
            return true;
        case P_DIRECTION:
            if (value >= StepEngine::SEQ_DIRECTIONS) return false;
            engine.set_direction(value);
            return true;
        case P_WIDTH:
            if (value == 0) return false;
            width_param = value;
            carry.set_width_us((uint32_t)value * 1000u);
            return true;
        default:
            return false;
    }
}

uint8_t Counter::get_param(uint16_t index) const {
    switch (index){
        case P_LENGTH:    return engine.length();
        case P_DIRECTION: return engine.direction();
        case P_WIDTH:     return width_param ? width_param : (uint8_t)(TRIGGER_WIDTH_US / 1000);
        default: return 0;
    }
}
