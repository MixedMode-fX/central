#include "node/param.h"

// Option names shared by more than one algorithm, defined once so two
// descriptors naming the same enum cannot disagree.

const char* const PARAM_DIRECTION_NAMES[5] = {
    "forward", "reverse", "pingpong", "random", "brownian",
};

// Index 0 is not a scale but a reference to the module's own (see
// midi/scale.h): it is what an algorithm carries until a user names a scale
// on it, which is why it reads as "global" rather than as a mode.
const char* const PARAM_SCALE_NAMES[15] = {
    "global", "major", "minor", "harmonic minor", "melodic minor",
    "pentatonic major", "pentatonic minor", "blues", "dorian", "phrygian",
    "lydian", "mixolydian", "locrian", "whole tone", "chromatic",
};

// global_scale::KeyFollow, in the enum's order. "follow" is zero because a
// node should be in the module's key until somebody says otherwise.
const char* const PARAM_KEY_NAMES[2] = {
    "follow", "own",
};

// NotePriorityRule, in the enum's order. Both algorithms that offer a
// priority use this table, whatever their parameter's own numbering.
const char* const PARAM_PRIORITY_NAMES[3] = {
    "lowest", "highest", "latest",
};
