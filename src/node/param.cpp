#include "node/param.h"

// Option names shared by more than one algorithm, defined once so two
// descriptors naming the same enum cannot disagree.

const char* const PARAM_DIRECTION_NAMES[5] = {
    "forward", "reverse", "pingpong", "random", "brownian",
};

const char* const PARAM_SCALE_NAMES[14] = {
    "chromatic", "major", "minor", "harmonic minor", "melodic minor",
    "pentatonic major", "pentatonic minor", "blues", "dorian", "phrygian",
    "lydian", "mixolydian", "locrian", "whole tone",
};
