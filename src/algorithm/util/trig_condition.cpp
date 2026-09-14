#include "algorithm/util/trig_condition.h"

const uint16_t TrigCondition::RATIO_CYCLE;
const uint16_t TrigCondition::N_PARAMS;

static const char* const CONDITION_NAMES[TrigCondition::COND_COUNT] = {
    "always", "first", "not first", "pre", "not pre", "nei", "not nei",
    "fill", "not fill",
};

// Written out rather than generated, because this is the list a user reads
// and the order is the parameter's value. It is sum(2..8) pairs after "off".
static const char* const RATIO_NAMES[TrigCondition::RATIO_COUNT] = {
    "off",
    "1:2", "2:2",
    "1:3", "2:3", "3:3",
    "1:4", "2:4", "3:4", "4:4",
    "1:5", "2:5", "3:5", "4:5", "5:5",
    "1:6", "2:6", "3:6", "4:6", "5:6", "6:6",
    "1:7", "2:7", "3:7", "4:7", "5:7", "6:7", "7:7",
    "1:8", "2:8", "3:8", "4:8", "5:8", "6:8", "7:8", "8:8",
};

const ParamDescriptor TrigCondition::PARAMS[TrigCondition::N_PARAMS] = {
    // 100 rather than 0, so an unconfigured node passes everything rather
    // than silencing the patch.
    {"chance",    1,          100,         100,        PARAM_PERCENT, nullptr},
    {"ratio",     RATIO_OFF,  RATIO_COUNT, RATIO_OFF,  PARAM_ENUM,    RATIO_NAMES},
    {"condition", COND_ALWAYS, COND_COUNT, COND_ALWAYS, PARAM_ENUM,   CONDITION_NAMES},
    {"seed",      0,          255,         0,          PARAM_NUMBER,  nullptr},
};

// X and Y for a stored ratio. Off is 1:1, which passes everything, so the
// caller does not have to special-case it.
void TrigCondition::ratio_pair(uint8_t stored, uint8_t& x, uint8_t& y){
    x = 1;
    y = 1;
    if (stored <= RATIO_OFF || stored > RATIO_COUNT) return;
    uint16_t i = (uint16_t)(stored - RATIO_OFF - 1);
    for (uint8_t candidate = 2; candidate <= RATIO_MAX_Y; candidate++){
        if (i < candidate){
            x = (uint8_t)(i + 1);
            y = candidate;
            return;
        }
        i = (uint16_t)(i - candidate);
    }
}

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

TrigCondition::TrigCondition(const uint8_t* params) :
    percent(params[0] ? params[0] : (uint8_t)100),
    ratio(clamp_enum(params[1], RATIO_COUNT, (uint8_t)RATIO_OFF)),
    condition(clamp_enum(params[2], COND_COUNT, (uint8_t)COND_ALWAYS)),
    seed_offset(params[3]),
    at(0), seen(false), last_pass(false),
    rng(entropy::seed() + params[3])
{}

// The odds and the rule move freely: a decision already taken is never
// revisited, so what a node has already passed stays passed. Writing the seed
// re-seeds, which is the point of a knob on it - two nodes at the same odds
// are made to disagree from the host.
bool TrigCondition::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: percent = value ? value : (uint8_t)100; return true;
        case 1:
            if (value == 0 || value > RATIO_COUNT) return false;
            ratio = value;
            return true;
        case 2:
            if (value == 0 || value > COND_COUNT) return false;
            condition = value;
            return true;
        case 3:
            if (value == seed_offset) return true;
            seed_offset = value;
            rng.reseed(entropy::seed() + seed_offset);
            return true;
        default:
            return false;
    }
}

uint8_t TrigCondition::get_param(uint16_t index) const {
    switch (index){
        case 0: return percent;
        case 1: return ratio;
        case 2: return condition;
        case 3: return seed_offset;
        default: return 0;
    }
}

void TrigCondition::restart(){
    at = 0;
    seen = false;
    last_pass = false;
}

bool TrigCondition::evaluate(bool fill, bool nei){
    const bool first = !seen;
    const uint16_t here = at;
    seen = true;
    // The count advances on every event that reaches the node, whatever the
    // rule decides about this one: a ratio is a position in a cycle, not a
    // tally of what got through, so switching the condition mid-pattern does
    // not shift where 2:4 falls.
    at = (uint16_t)((at + 1u) % RATIO_CYCLE);

    bool ok;
    switch (condition){
        case COND_FIRST:     ok = first;      break;
        case COND_NOT_FIRST: ok = !first;     break;
        case COND_PRE:       ok = last_pass;  break;
        case COND_NOT_PRE:   ok = !last_pass; break;
        case COND_NEI:       ok = nei;        break;
        case COND_NOT_NEI:   ok = !nei;       break;
        case COND_FILL:      ok = fill;       break;
        case COND_NOT_FILL:  ok = !fill;      break;
        default:             ok = true;       break;   // COND_ALWAYS
    }

    if (ok && ratio > RATIO_OFF){
        uint8_t x = 1;
        uint8_t y = 1;
        ratio_pair(ratio, x, y);
        ok = (uint8_t)(here % y) == (uint8_t)(x - 1);
    }

    // Last, and only on an event the rule already allowed: the dice are the
    // cheapest of the three and the only one with a stream to keep, so an
    // event the condition rejected does not spend one.
    if (ok) ok = rng.chance(percent);

    last_pass = ok;
    return ok;
}
