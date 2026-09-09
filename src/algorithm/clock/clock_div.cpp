#include "algorithm/clock/clock_div.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::Gate};

const AlgorithmDescriptor ClockDiv::descriptor = {
    ALGO_CLOCK_DIV, "ClockDiv", 1, 0, 1, 5, IN, OUT, sizeof(ClockDiv), true, construct_node<ClockDiv> };

ClockDiv::ClockDiv(const NodeConfig& config) :
    source_in(config.in_bus[0]),
    out(config.out_bus[0]),
    mode(config.params[0]),
    amount(config.params[1] ? config.params[1] : 1),
    div_period(1), offset(0), next_fire(0), last_position(0),
    edges(0), pulse_count(0), now(0), pulse(),
    refused(false), started(false), last_gate(false)
{
    const bool from_gate = (source_in != NO_BUS);
    const bool multiply = (mode != 0);

    if (multiply && from_gate){
        refused = true;                       // documented above: x1 instead
        div_period = 1;
    } else if (multiply){
        // Exact only when the multiplier divides the subdivision. Anything
        // else would land pulses on fractional subticks, so it is refused the
        // same way multiplication from a gate source is.
        if (amount <= CLOCK_SUBTICK && (CLOCK_SUBTICK % amount) == 0){
            div_period = (uint32_t)CLOCK_SUBTICK / amount;
        } else {
            refused = true;
            div_period = CLOCK_SUBTICK;
        }
    } else {
        div_period = from_gate ? (uint32_t)amount : (uint32_t)amount * CLOCK_SUBTICK;
    }
    if (div_period == 0) div_period = 1;

    // phase is a fraction of one output period; delay is whole ticks (whole
    // input edges from a gate source).
    const uint32_t phase = ((uint32_t)config.params[2] * div_period) / 256u;
    const uint32_t delay = from_gate ? (uint32_t)config.params[3]
                                     : (uint32_t)config.params[3] * CLOCK_SUBTICK;
    offset = phase + delay;
    next_fire = offset;

    if (config.params[4]) pulse.set_width_us((uint32_t)config.params[4] * 1000u);
}

void ClockDiv::restart(){
    next_fire = offset;
    edges = 0;
    last_position = 0;
    started = false;
    pulse.clear();
}

void ClockDiv::fire(BusManager& bus){
    pulse.fire(now);
    pulse_count++;
    bus.gate_write(out, true);
}

void ClockDiv::advance_to(BusManager& bus, uint32_t position){
    if (started && position < last_position) restart();   // the clock restarted
    if (!started){
        started = true;
        // A node loaded into a patch that has been running for an hour joins
        // the pattern where the pattern is, rather than firing on the subtick
        // it happened to be constructed on and running half a step out.
        if (position > next_fire){
            const uint32_t elapsed = position - next_fire;
            uint32_t skip = elapsed / div_period;
            if (elapsed % div_period) skip++;
            next_fire += skip * div_period;
        }
    }
    last_position = position;

    if (position < next_fire) return;
    fire(bus);
    // Skip whole periods rather than looping, so a node loaded into a patch
    // that has been running for an hour costs the same as one loaded at zero.
    const uint32_t elapsed = position - next_fire;
    next_fire += (elapsed / div_period + 1u) * div_period;
}

void ClockDiv::process(BusManager& bus, uint32_t now_us){
    now = now_us;

    if (source_in != NO_BUS){
        const bool level = bus.gate_read(source_in);
        const bool rising = level && !last_gate;
        last_gate = level;
        if (rising){
            edges++;
            advance_to(bus, edges - 1u);   // the first edge is position 0
        }
    }

    // Hold the pulse up for its full width, whichever source fired it.
    if (pulse.level(now_us)) bus.gate_write(out, true);
}

void ClockDiv::tick(BusManager& bus, uint32_t count){
    if (source_in != NO_BUS) return;        // gate-sourced: the tick is not ours
    advance_to(bus, count);
}
