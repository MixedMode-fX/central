#ifndef MMMC_CLOCK_MUSICAL_DIVISION_H
#define MMMC_CLOCK_MUSICAL_DIVISION_H

#include <stdint.h>
#include "config.h"

// Note values, and what they are worth in subticks.
//
// These started inside Metronome, which was the only thing that named a rate
// in musical terms. An LFO synced to the clock needs exactly the same list -
// and a user who has set a Metronome to "1/16 triplet" is entitled to have
// that mean the identical number of subticks when they set an LFO to it. So
// the vocabulary lives here, once, and both algorithms select from it.
//
// Slowest first, indexed from 1 so that a stored 0 can still mean the
// parameter descriptor's default (node/param.h). A "bar" is four quarter
// notes: the module has no time signature, and 4/4 is the only reading of
// "bar" that needs no other information.
enum MusicalDivision : uint8_t {
    DIV_8_BARS  = 1,
    DIV_4_BARS  = 2,
    DIV_2_BARS  = 3,
    DIV_BAR     = 4,
    DIV_HALF    = 5,
    DIV_QUARTER = 6,
    DIV_EIGHTH  = 7,
    DIV_16TH    = 8,
    DIV_32ND    = 9,
    DIV_64TH    = 10,
    DIVISIONS   = 10,
};

// What the note value is worth. Straight is the value itself, dotted is half
// as long again, a triplet is three in the space of two.
enum MusicalFeel : uint8_t {
    FEEL_STRAIGHT = 1,
    FEEL_DOTTED   = 2,
    FEEL_TRIPLET  = 3,
    FEELS         = 3,
};

// Option names for the two enums, so every algorithm that offers them offers
// the same words in the same order (node/param.h's PARAM_ENUM).
extern const char* const DIVISION_NAMES[DIVISIONS];
extern const char* const FEEL_NAMES[FEELS];

// Subticks in one division at one feel. **Every combination is a whole
// number**, which is the claim the friendly control rests on: a musician who
// picks "1/16 triplet" gets the same exactness as one who worked out `/2` on
// the divider, and never a rate that drifts by a subtick a bar. The
// static_assert in the .cpp is what keeps it one.
//
// Out-of-range arguments are clamped to the default (a quarter, straight)
// rather than returning zero: this is called from derive() paths that would
// divide by it.
uint32_t division_subticks(uint8_t division, uint8_t feel);

#endif
