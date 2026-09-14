#include "algorithm/util/gate_probability.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[3] = {Domain::Gate, Domain::Gate, Domain::Gate};

static const ParamGroup GROUPS[1] = {
    {0, 1, TrigCondition::N_PARAMS, TrigCondition::PARAMS, "chance"},
};

static const char* const IN_NAMES[2] = {"gate in", "reset"};
static const char* const OUT_NAMES[3] = {"gate out", "dropped", "passed"};

const AlgorithmDescriptor GateProbability::descriptor = {
    ALGO_GATE_PROBABILITY, "GateProbability", 2, 1, 3, TrigCondition::N_PARAMS, IN, OUT,
    sizeof(GateProbability), false, construct_node<GateProbability>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Lets each gate through under a chance and a condition, decided on its rising edge.",
    CATEGORY_UTILITY };

bool GateProbability::set_param(uint16_t index, uint8_t value){
    return condition.set_param(index, value);
}

uint8_t GateProbability::get_param(uint16_t index) const {
    return condition.get_param(index);
}

GateProbability::GateProbability(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    dropped_out(config.out_bus[1]),
    passed_out(config.out_bus[2]),
    reset_in(config.in_bus[1]),
    condition(config.params),
    last_in(false),
    passing(false)
{}

void GateProbability::process(BusManager& bus, uint32_t){
    // Before the edge, so a reset and a rising edge in one pass play the
    // first event of the new count rather than the last of the old one.
    if (reset_in.rising(bus)) condition.reset();

    const bool level = bus.gate_read(in);
    if (level && !last_in) passing = condition.evaluate();
    // The decision dies with the gate that carried it, so the next edge asks
    // again rather than inheriting the last answer.
    if (!level) passing = false;
    last_in = level;

    if (passing) bus.gate_write(out, true);
    // The other half of the same gate: high while an input gate the rule
    // refused is up, and never between two of them.
    if (dropped_out != NO_BUS && level && !passing) bus.gate_write(dropped_out, true);
    // Latched, not a pulse: a reader clocked in some other pass has to see
    // the last decision rather than nothing.
    if (passed_out != NO_BUS && condition.passed()) bus.gate_write(passed_out, true);
}
