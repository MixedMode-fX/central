#include "algorithm/util/gate_hold.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Gate};

static const char* const MODE_NAMES[GateHold::HOLD_MODES] = {
    "latch", "toggle", "extend", "limit",
};

static const ParamDescriptor PARAMS[3] = {
    {"mode", GateHold::HOLD_LATCH, GateHold::HOLD_MODES, GateHold::HOLD_LATCH, PARAM_ENUM, MODE_NAMES},
    {"hold", 1, 255, GateHold::DEFAULT_HOLD_MS, PARAM_MILLIS, nullptr},
    {"gate", 0, 1, 0, PARAM_BOOL, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 3, PARAMS}};

static const char* const IN_NAMES[2] = {"set", "reset"};
static const char* const OUT_NAMES[1] = {"gate"};

// min_in is 0: with nothing patched at all the node is a switch, and its
// `gate` parameter is the level it sends (see the header).
const AlgorithmDescriptor GateHold::descriptor = {
    ALGO_GATE_HOLD, "GateHold", 2, 0, 1, 3, IN, OUT, sizeof(GateHold), false, construct_node<GateHold>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Holds a gate up: a switch that stays put, a latch, a toggle, or a trigger stretched to length." };

static uint8_t clamp_mode(uint8_t stored){
    if (stored == 0) return GateHold::HOLD_LATCH;
    return stored > GateHold::HOLD_MODES ? (uint8_t)GateHold::HOLD_LATCH : stored;
}

GateHold::GateHold(const NodeConfig& config) :
    set_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    out(config.out_bus[0]),
    how(clamp_mode(config.params[0])),
    hold_param(config.params[1]),
    // A patch that stored the gate up loads with it up: the level is state a
    // user set, not something to be re-derived from an input that may not
    // even be patched.
    since_us(0), timing(false), level(config.params[P_GATE] != 0),
    last_set(false), last_reset(false)
{}

bool GateHold::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > HOLD_MODES) return false;
            if (value == how) return true;
            // Deliberately not a reset: see the class comment.
            how = value;
            return true;
        case 1:
            if (value == 0) return false;       // the descriptor's minimum is 1
            hold_param = value;
            return true;
        case P_GATE:
            if (value > 1) return false;
            // The same level an edge on `set` or `reset` moves, so a switch
            // and a cable are one control rather than two. The timed modes
            // recompute it from their input on the next pass.
            level = value != 0;
            if (!level) timing = false;
            return true;
        default:
            return false;
    }
}

uint8_t GateHold::get_param(uint16_t index) const {
    switch (index){
        case 0: return how;
        case 1: return hold_param;
        case P_GATE: return level ? 1u : 0u;
        default: return 0;
    }
}

void GateHold::process(BusManager& bus, uint32_t now_us){
    const bool set_level = (set_in == NO_BUS) ? false : bus.gate_read(set_in);
    const bool set_rise = set_level && !last_set;
    last_set = set_level;

    bool reset_rise = false;
    if (reset_in != NO_BUS){
        const bool r = bus.gate_read(reset_in);
        reset_rise = r && !last_reset;
        last_reset = r;
    }

    switch (how){
        case HOLD_TOGGLE:
            if (set_rise) level = !level;
            break;

        case HOLD_EXTEND:
            // Retrigger restarts the timer, so a burst of triggers holds the
            // gate up throughout rather than chopping it.
            if (set_rise){ timing = true; since_us = now_us; }
            if (timing && (uint32_t)(now_us - since_us) >= hold_us()) timing = false;
            level = set_level || timing;
            break;

        case HOLD_LIMIT:
            // The window opens on the rising edge and closes when the input
            // falls, so a gate that outlasts `hold` cannot re-open the output
            // without going low first.
            if (set_rise){ timing = true; since_us = now_us; }
            if (!set_level) timing = false;
            if (timing && (uint32_t)(now_us - since_us) >= hold_us()) timing = false;
            level = set_level && timing;
            break;

        default:                                // HOLD_LATCH
            if (set_rise) level = true;
            break;
    }

    if (reset_rise){ level = false; timing = false; }
    if (level) bus.gate_write(out, true);
}
