#include "algorithm/sequencer/gate_sequencer.h"

GateSequencer::GateSequencer(const NodeConfig& config, uint8_t default_length) :
    engine(),
    rng(entropy::seed()),
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    extra_in(config.in_bus[2]),
    out(config.out_bus[0]),
    pulse(),
    chance()
{
    engine.configure(config.params[0], config.params[1], default_length);
    if (config.params[2]) pulse.set_width_us((uint32_t)config.params[2] * 1000u);
    for (uint8_t i = 0; i < MAX_SEQUENCE_LEN; i++) chance[i] = step_probability(config.params[PROBABILITY_BASE + i]);
}

uint32_t GateSequencer::pattern() const {
    uint32_t bits = 0;
    for (uint8_t i = 0; i < length(); i++) if (step_on(i)) bits |= (uint32_t)1u << i;
    return bits;
}

void GateSequencer::process(BusManager& bus, uint32_t now_us){
    if (reset_in.rising(bus)){ engine.reset(); on_reset(); }
    if (extra_in.rising(bus)) on_extra_edge();

    if (advance_in.rising(bus)){
        const uint8_t step = engine.advance(rng);
        if (step_on(step) && rng.chance(chance[step])) pulse.fire(now_us);
    }
    if (pulse.level(now_us)) bus.gate_write(out, true);
}
