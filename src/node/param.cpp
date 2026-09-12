#include "node/param.h"

// Option names shared by more than one algorithm, defined once so two
// descriptors naming the same enum cannot disagree.

const char* const PARAM_DIRECTION_NAMES[5] = {
    "forward", "reverse", "pingpong", "random", "brownian",
};

// Index 0 is not a scale but an unset byte (see midi/scale.h), which is why
// it reads as "none" rather than as a mode.
const char* const PARAM_SCALE_NAMES[15] = {
    "none", "major", "minor", "harmonic minor", "melodic minor",
    "pentatonic major", "pentatonic minor", "blues", "dorian", "phrygian",
    "lydian", "mixolydian", "locrian", "whole tone", "chromatic",
};

// Index 0 is the key's register rather than an octave: see param.h.
const char* const PARAM_OCTAVE_NAMES[11] = {
    "key", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
};

// NotePriorityRule, in the enum's order. Both algorithms that offer a
// priority use this table, whatever their parameter's own numbering.
const char* const PARAM_PRIORITY_NAMES[3] = {
    "lowest", "highest", "latest",
};
