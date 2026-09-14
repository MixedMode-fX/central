#include "algorithm/util/gate_probability.h"
#include "node/registry.h"
#include "clock/transport_edge.h"

static const Domain IN[3] = {Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Gate, Domain::Gate};

static const ParamGroup GROUPS[1] = {
    {0, 1, TrigCondition::N_PARAMS, TrigCondition::PARAMS, "chance"},
};

static const char* const IN_NAMES[3] = {"gate in", "fill", "nei"};
static const char* const OUT_NAMES[2] = {"gate out", "passed"};

const AlgorithmDescriptor GateProbability::descriptor = {
    ALGO_GATE_PROBABILITY, "GateProbability", 3, 1, 2, TrigCondition::N_PARAMS, IN, OUT,
    sizeof(GateProbability), false, construct_node<GateProbability>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Lets each gate through under a chance, a ratio and a condition, decided on its rising edge.",
    CATEGORY_UTILITY };

bool GateProbability::set_param(uint16_t index, uint8_t value){
    return condition.set_param(index, value);
}

uint8_t GateProbability::get_param(uint16_t index) const {
    return condition.get_param(index);
}

GateProbability::GateProbability(const NodeConfig& config) :
    in(config.in_bus[0]),
    fill_in(config.in_bus[1]),
    nei_in(config.in_bus[2]),
    out(config.out_bus[0]),
    passed_out(config.out_bus[1]),
    condition(config.params),
    last_in(false),
    passing(false)
{}

void GateProbability::process(BusManager& bus, uint32_t){
    const bool level = bus.gate_read(in);

    if (level && !last_in){
        const bool fill = (fill_in == NO_BUS) ? false : bus.gate_read(fill_in);
        const bool nei = (nei_in == NO_BUS) ? false : bus.gate_read(nei_in);
        passing = condition.evaluate(fill, nei);
    }
    // The decision dies with the gate that carried it, so the next edge asks
    // again rather than inheriting the last answer.
    if (!level) passing = false;
    last_in = level;

    if (passing) bus.gate_write(out, true);
    // Latched, not a pulse: the neighbour reading this has to see the last
    // decision whether or not it is clocked in the same pass as this one.
    if (passed_out != NO_BUS && condition.passed()) bus.gate_write(passed_out, true);
}

// A start puts the count back on the downbeat, so `first` and a ratio mean
// the same thing every time the pattern is played from the top. A continue
// is the message that means "where we left off" and leaves it alone.
void GateProbability::transport_event(BusManager&, uint8_t edges){
    if (edges & TRANSPORT_START) condition.restart();
}
