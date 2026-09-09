#include "algorithm/sequencer/gate_sequencer.h"

GateSequencer::GateSequencer(const NodeConfig& config, uint8_t default_length) :
    len(config.params[0] ? config.params[0] : default_length),
    rng(entropy::seed()),
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    extra_in(config.in_bus[2]),
    out(config.out_bus[0]),
    direction(config.params[1]),
    cursor(0), steps(0), pulse(),
    at_first(true), descending(false),
    last_advance(false), last_reset(false), last_extra(false)
{
    if (len == 0) len = 1;
    if (len > MAX_SEQUENCE_LEN) len = MAX_SEQUENCE_LEN;
    if (config.params[2]) pulse.set_width_us((uint32_t)config.params[2] * 1000u);
}

uint32_t GateSequencer::pattern() const {
    uint32_t bits = 0;
    for (uint8_t i = 0; i < len; i++) if (step_on(i)) bits |= (uint32_t)1u << i;
    return bits;
}

void GateSequencer::reset_sequence(){
    at_first = true;
    descending = false;
    on_reset();
}

// The first step after a reset is the pattern's first step - step 0 going
// forwards, the last step going backwards - and the direction only starts
// counting from the step after that.
void GateSequencer::step_forward(){
    if (at_first){
        at_first = false;
        cursor = (direction == SEQ_REVERSE) ? (uint8_t)(len - 1u) : 0;
        return;
    }
    switch (direction){
        case SEQ_REVERSE:
            cursor = cursor == 0 ? (uint8_t)(len - 1u) : (uint8_t)(cursor - 1u);
            break;
        case SEQ_PINGPONG:
            // Endpoints are not repeated: 0 1 2 3 2 1 0 1 ...
            if (len == 1){ cursor = 0; break; }
            if (descending){
                if (cursor == 0){ descending = false; cursor = 1; }
                else cursor--;
            } else {
                if (cursor + 1u >= len){ descending = true; cursor = (uint8_t)(len - 2u); }
                else cursor++;
            }
            break;
        case SEQ_RANDOM:
            cursor = rng.below(len);
            break;
        default:
            cursor = (uint8_t)((cursor + 1u) % len);
            break;
    }
}

void GateSequencer::process(BusManager& bus, uint32_t now_us){
    if (reset_in != NO_BUS){
        const bool level = bus.gate_read(reset_in);
        if (level && !last_reset) reset_sequence();
        last_reset = level;
    }
    if (extra_in != NO_BUS){
        const bool level = bus.gate_read(extra_in);
        if (level && !last_extra) on_extra_edge();
        last_extra = level;
    }

    const bool level = bus.gate_read(advance_in);
    const bool rising = level && !last_advance;
    last_advance = level;

    if (rising){
        step_forward();
        steps++;
        if (step_on(cursor)) pulse.fire(now_us);
    }
    if (pulse.level(now_us)) bus.gate_write(out, true);
}
