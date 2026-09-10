#include "clock/musical_division.h"

const char* const DIVISION_NAMES[DIVISIONS] = {
    "8 bars", "4 bars", "2 bars", "1 bar",
    "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
};

const char* const FEEL_NAMES[FEELS] = {
    "straight", "dotted", "triplet",
};

// Subticks in one of each note value. A bar is four quarters (the header says
// why), so the table is the quarter scaled by powers of two and nothing here
// depends on a time signature the module does not have.
static const uint32_t DIVISION_SUBTICKS[DIVISIONS] = {
    CLOCK_SUBTICKS_PER_QUARTER * 32u,      // 8 bars
    CLOCK_SUBTICKS_PER_QUARTER * 16u,      // 4 bars
    CLOCK_SUBTICKS_PER_QUARTER * 8u,       // 2 bars
    CLOCK_SUBTICKS_PER_QUARTER * 4u,       // 1 bar
    CLOCK_SUBTICKS_PER_QUARTER * 2u,       // 1/2
    CLOCK_SUBTICKS_PER_QUARTER,            // 1/4
    CLOCK_SUBTICKS_PER_QUARTER / 2u,       // 1/8
    CLOCK_SUBTICKS_PER_QUARTER / 4u,       // 1/16
    CLOCK_SUBTICKS_PER_QUARTER / 8u,       // 1/32
    CLOCK_SUBTICKS_PER_QUARTER / 16u,      // 1/64
};

// The fastest value is the binding one. A 1/64 is a quarter over sixteen; a
// dotted 1/64 is three quarters over thirty-two, and a 1/64 triplet is a
// quarter over twenty-four. Both are exact when the quarter is a multiple of
// 96, which at CLOCK_SUBTICK = 24 it is by a factor of six.
static_assert(CLOCK_SUBTICKS_PER_QUARTER % 96u == 0,
              "a note value would not be a whole number of subticks; see config.h");

uint32_t division_subticks(uint8_t division, uint8_t feel){
    if (division < DIV_8_BARS || division > DIVISIONS) division = DIV_QUARTER;
    if (feel < FEEL_STRAIGHT || feel > FEELS) feel = FEEL_STRAIGHT;
    const uint32_t base = DIVISION_SUBTICKS[division - DIV_8_BARS];
    switch (feel){
        case FEEL_DOTTED:  return base * 3u / 2u;
        case FEEL_TRIPLET: return base * 2u / 3u;
        default:           return base;
    }
}
