#include "algorithm/util/trig_condition.h"

const uint8_t TrigCondition::RATIO_MAX_Y;
const uint16_t TrigCondition::RATIO_CYCLE;
const uint16_t TrigCondition::N_PARAMS;

// Written out rather than generated, because this is the list a user reads
// and its order is the parameter's value: `always`, the two one-shots, then
// sum(2..RATIO_MAX_Y) pairs.
static const char* const CONDITION_NAMES[TrigCondition::COND_COUNT] = {
    "always", "first", "not first",
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
    {"chance",    1,           100,        100,         PARAM_PERCENT, nullptr},
    {"condition", COND_ALWAYS, COND_COUNT, COND_ALWAYS, PARAM_ENUM,    CONDITION_NAMES},
    {"seed",      0,           255,        0,           PARAM_NUMBER,  nullptr},
};

void TrigCondition::ratio_pair(uint8_t stored, uint8_t& x, uint8_t& y){
    x = 1;
    y = 1;
    if (stored < COND_RATIO || stored > COND_COUNT) return;
    uint16_t i = (uint16_t)(stored - COND_RATIO);
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
    condition(clamp_enum(params[1], COND_COUNT, (uint8_t)COND_ALWAYS)),
    seed_offset(params[2]),
    at(0), seen(false), last_pass(false),
    rng(entropy::seed() + params[2])
{}

// The odds and the rule move freely: a decision already taken is never
// revisited, so what a node has already passed stays passed. Writing the seed
// re-seeds, which is the point of a knob on it - two nodes at the same odds
// are made to disagree from the host.
bool TrigCondition::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: percent = value ? value : (uint8_t)100; return true;
        case 1:
            if (value == 0 || value > COND_COUNT) return false;
            condition = value;
            return true;
        case 2:
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
        case 1: return condition;
        case 2: return seed_offset;
        default: return 0;
    }
}

void TrigCondition::reset(){
    at = 0;
    seen = false;
    last_pass = false;
}

bool TrigCondition::evaluate(){
    const bool first = !seen;
    const uint16_t here = at;
    seen = true;
    // The count advances on every event that reaches the node, whatever the
    // rule decides about this one: a ratio is a position in a cycle, not a
    // tally of what got through, so changing the condition mid-pattern does
    // not shift where 2:4 falls.
    at = (uint16_t)((at + 1u) % RATIO_CYCLE);

    bool ok;
    if (condition == COND_FIRST){
        ok = first;
    } else if (condition == COND_NOT_FIRST){
        ok = !first;
    } else if (condition >= COND_RATIO){
        uint8_t x = 1;
        uint8_t y = 1;
        ratio_pair(condition, x, y);
        ok = (uint8_t)(here % y) == (uint8_t)(x - 1);
    } else {
        ok = true;                              // COND_ALWAYS
    }

    // Last, and only on an event the condition already allowed: the dice are
    // the only one of the two with a stream to keep, so an event the count
    // rejected does not spend one.
    if (ok) ok = rng.chance(percent);

    last_pass = ok;
    return ok;
}
