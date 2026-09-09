#include "algorithm/sequencer/gate_sequencer.h"

const ParamDescriptor GateSequencer::HEADER[3] = {
    {"length",    1, MAX_SEQUENCE_LEN,           8, PARAM_NUMBER, nullptr},
    {"direction", 0, StepEngine::SEQ_DIRECTIONS - 1, 0, PARAM_ENUM, PARAM_DIRECTION_NAMES},
    {"width",     0, 255,                        0, PARAM_MILLIS, nullptr},
};

const ParamDescriptor GateSequencer::PROBABILITY[1] = {
    {"probability", 0, 100, 100, PARAM_PERCENT, nullptr},
};

const ParamDescriptor GateSequencer::RESERVED[1] = {
    {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
};

GateSequencer::GateSequencer(const NodeConfig& config, uint8_t default_length) :
    engine(),
    rng(entropy::seed()),
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    extra_in(config.in_bus[2]),
    out(config.out_bus[0]),
    fallback(default_length ? default_length : 1),
    width_param(config.params[2]),
    pulse(),
    chance()
{
    engine.configure(config.params[0], config.params[1], fallback);
    if (config.params[2]) pulse.set_width_us((uint32_t)config.params[2] * 1000u);
    for (uint8_t i = 0; i < MAX_SEQUENCE_LEN; i++) chance[i] = step_probability(config.params[PROBABILITY_BASE + i]);
}

bool GateSequencer::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: {
            const uint8_t want = value ? value : fallback;
            if (want == engine.length()) return true;
            engine.set_length(value, fallback);   // cursor clamps on the next advance
            on_length_changed();
            return true;
        }
        case 1:
            if (value >= StepEngine::SEQ_DIRECTIONS) return false;
            engine.set_direction(value);
            return true;
        case 2:
            width_param = value;
            pulse.set_width_us(value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US);
            return true;
        default:
            break;
    }
    if (index >= PROBABILITY_BASE && index < PARAM_COUNT){
        if (value > 100) return false;
        chance[index - PROBABILITY_BASE] = step_probability(value);
        return true;
    }
    return false;      // params[3..7] belong to the subclass
}

uint8_t GateSequencer::get_param(uint16_t index) const {
    switch (index){
        case 0: return engine.length();
        case 1: return engine.direction();
        case 2: return width_param;
        default: break;
    }
    if (index >= PROBABILITY_BASE && index < PARAM_COUNT) return chance[index - PROBABILITY_BASE];
    return 0;
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
