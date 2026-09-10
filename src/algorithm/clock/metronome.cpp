#include "algorithm/clock/metronome.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::Gate};

// The note values, as an editor lists them: slowest first, so the control
// reads like a tempo control rather than like a divisor. The names and the
// table are shared with every other algorithm that offers a note value
// (clock/musical_division.h); indexed from the enum's minimum, which is 1 -
// see the enum for why it is not 0.
static const ParamDescriptor PARAMS[3] = {
    {"division", DIV_8_BARS,    DIVISIONS, DIV_QUARTER,   PARAM_ENUM, DIVISION_NAMES},
    {"feel",     FEEL_STRAIGHT, FEELS,     FEEL_STRAIGHT, PARAM_ENUM, FEEL_NAMES},
    {"width",    0, 255, 0, PARAM_MILLIS, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 3, PARAMS}};

static const char* const IN_NAMES[1] = {"reset"};
static const char* const OUT_NAMES[1] = {"trigger"};

const AlgorithmDescriptor Metronome::descriptor = {
    ALGO_METRONOME, "Metronome", 1, 0, 1, 3, IN, OUT, sizeof(Metronome), true, construct_node<Metronome>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "The clock as note values: 1/4, 1/8, dotted, triplet. A divider you do not have to count.",
    CATEGORY_CLOCK };

static uint8_t clamp_division(uint8_t stored){
    if (stored == 0) return DIV_QUARTER;                 // a zeroed preset is the beat
    return stored > DIVISIONS ? (uint8_t)DIVISIONS : stored;
}

static uint8_t clamp_feel(uint8_t stored){
    if (stored == 0) return FEEL_STRAIGHT;
    return stored > FEELS ? (uint8_t)FEEL_STRAIGHT : stored;
}

Metronome::Metronome(const NodeConfig& config) :
    reset_in(config.in_bus[0]),
    out(config.out_bus[0]),
    div(clamp_division(config.params[0])),
    how(clamp_feel(config.params[1])),
    width_param(config.params[2]),
    div_period(1), next_fire(0), last_position(0), pulse_count(0), now(0),
    pulse(), started(false), last_gate(false), pending_reset(false)
{
    derive();
    if (width_param) pulse.set_width_us((uint32_t)width_param * 1000u);
}

void Metronome::derive(){
    const uint32_t p = division_subticks(div, how);
    div_period = p ? p : 1u;
}

bool Metronome::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > DIVISIONS) return false;
            if (value == div) return true;
            div = value;
            break;
        case 1:
            if (value == 0 || value > FEELS) return false;
            if (value == how) return true;
            how = value;
            break;
        case 2:
            width_param = value;
            pulse.set_width_us(value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US);
            return true;
        default:
            return false;
    }
    derive();
    return true;
}

uint8_t Metronome::get_param(uint16_t index) const {
    switch (index){
        case 0: return div;
        case 1: return how;
        case 2: return width_param;
        default: return 0;
    }
}

void Metronome::restart(){
    next_fire = 0;
    last_position = 0;
    started = false;
    pulse.clear();
}

void Metronome::fire(BusManager& bus){
    pulse.fire(now);
    pulse_count++;
    bus.gate_write(out, true);
}

void Metronome::advance_to(BusManager& bus, uint32_t position){
    if (started && position < last_position) restart();   // the clock restarted
    if (!started){
        started = true;
        // The grid is anchored on subtick 0 - the downbeat MasterClock::start()
        // resets to - so a metronome added to a patch that has been running
        // for an hour lands on the beat rather than on the subtick it happened
        // to be constructed on.
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
    const uint32_t elapsed = position - next_fire;
    next_fire += (elapsed / div_period + 1u) * div_period;
}

void Metronome::process(BusManager& bus, uint32_t now_us){
    now = now_us;

    if (reset_in != NO_BUS){
        const bool level = bus.gate_read(reset_in);
        if (level && !last_gate) pending_reset = true;
        last_gate = level;
    }

    // Hold the pulse up for its full width, whatever fired it.
    if (pulse.level(now_us)) bus.gate_write(out, true);
}

void Metronome::tick(BusManager& bus, uint32_t count){
    if (pending_reset){
        // The reset edge is a downbeat: pulse here and count the next
        // division from here. Applied on the subtick rather than in
        // process(), so the grid re-anchors on a position the node knows -
        // at most one subtick of lag, which is 347 us at CLOCK_MAX_BPM and
        // two orders of magnitude inside a trigger.
        pending_reset = false;
        started = true;
        last_position = count;
        next_fire = count + div_period;
        fire(bus);
        return;
    }
    advance_to(bus, count);
}
