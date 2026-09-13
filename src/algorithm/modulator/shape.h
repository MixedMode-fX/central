#ifndef MMMC_ALGORITHM_MODULATOR_SHAPE_H
#define MMMC_ALGORITHM_MODULATOR_SHAPE_H

#include <stdint.h>
#include "bus/domain.h"

// The curve a modulator draws, and the one table it is drawn from.
//
// Two nodes read a shape at a phase and differ only in what moves the phase:
// Lfo moves it with time, StepMod moves it with triggers. That is the same
// argument algorithm/sequencer/step_engine.h makes for the sequencers - an
// LFO and a StepMod both set to "triangle" are the same curve by
// construction, rather than by two tables being kept in step by hand.
//
// Indexed from 1, so a stored 0 still means the descriptor's default.
enum CvShape : uint8_t {
    CV_SHAPE_SINE         = 1,
    CV_SHAPE_TRIANGLE     = 2,
    CV_SHAPE_RAMP_UP      = 3,
    CV_SHAPE_RAMP_DOWN    = 4,
    CV_SHAPE_SQUARE       = 5,
    CV_SHAPE_RANDOM_STEP  = 6,   // a level, held until the node draws again
    CV_SHAPE_RANDOM_GLIDE = 7,   // ... slid into across the phase
    CV_SHAPES             = 7,
};

extern const char* const CV_SHAPE_NAMES[CV_SHAPES];

// The raw shape, 0 .. CV_MAX, at a phase 0 .. CV_MAX.
//
// `from` and `to` are the two random levels the random shapes read. **When a
// node draws them is the node's own business**, and it is the entire
// difference between one random level per LFO cycle and one per trigger.
uint16_t cv_shape_at(uint8_t shape, uint16_t phase, uint16_t from, uint16_t to);

// Applies depth, offset and polarity to a raw shape value.
//
// The offset is a signed byte read as a fraction of half of full scale, so it
// can move a bipolar shape from one rail to the other.
int16_t cv_shape_scaled(uint16_t raw, uint8_t depth, uint8_t offset_param, bool unipolar);

#endif
