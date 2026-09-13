#include "algorithm/modulator/shape.h"

const char* const CV_SHAPE_NAMES[CV_SHAPES] = {
    "sine", "triangle", "ramp up", "ramp down", "square", "random step", "random glide",
};

// A quarter of a sine, 65 points at twelve bits, interpolated between. The
// other three quarters are this one reflected, so the table is 130 bytes and
// there is no floating point anywhere in the signal path.
static const uint16_t SINE_QUARTER[65] = {
       0,  100,  201,  301,  401,  501,  601,  700,
     799,  897,  995, 1092, 1189, 1285, 1380, 1474,
    1567, 1659, 1751, 1841, 1930, 2018, 2105, 2191,
    2275, 2358, 2439, 2519, 2598, 2675, 2750, 2824,
    2896, 2966, 3034, 3101, 3165, 3228, 3289, 3348,
    3405, 3460, 3512, 3563, 3611, 3658, 3702, 3744,
    3783, 3821, 3856, 3888, 3919, 3947, 3972, 3996,
    4016, 4035, 4051, 4064, 4075, 4084, 4090, 4094,
    4095,
};

// sin(x * pi/2 / 1024) at twelve bits, for x in 0 .. 1024.
static uint16_t sine_quarter(uint32_t x){
    if (x >= 1024u) return SINE_QUARTER[64];
    const uint32_t index = x >> 4;
    const uint32_t frac = x & 15u;
    const uint32_t a = SINE_QUARTER[index];
    const uint32_t b = SINE_QUARTER[index + 1];
    return (uint16_t)(a + ((b - a) * frac) / 16u);
}

uint16_t cv_shape_at(uint8_t shape, uint16_t phase, uint16_t from, uint16_t to){
    switch (shape){
        case CV_SHAPE_TRIANGLE:
            // Bottom at the start of the cycle, top at the half, which is how
            // a triangle is drawn on every module that has one.
            return phase < CV_HALF ? (uint16_t)(phase * 2u)
                                   : (uint16_t)(CV_FULL * 2u - 1u - (uint32_t)phase * 2u);
        case CV_SHAPE_RAMP_UP:
            return phase;
        case CV_SHAPE_RAMP_DOWN:
            return (uint16_t)(CV_MAX - phase);
        case CV_SHAPE_SQUARE:
            return phase < CV_HALF ? (uint16_t)CV_MAX : (uint16_t)0;
        case CV_SHAPE_RANDOM_STEP:
            return to;
        case CV_SHAPE_RANDOM_GLIDE:
            return (uint16_t)((int32_t)from
                 + ((int32_t)to - (int32_t)from) * (int32_t)phase / CV_FULL);
        default: {
            // Sine, from the quarter table. Starts at the centre going up, so
            // a bipolar sine leaves zero rising - the shape a musician draws
            // when they say "sine".
            const uint32_t quarter = (uint32_t)phase >> 10;
            const uint32_t within = (uint32_t)phase & 0x3FFu;
            int32_t s;
            switch (quarter){
                case 0:  s =  (int32_t)sine_quarter(within); break;
                case 1:  s =  (int32_t)sine_quarter(1024u - within); break;
                case 2:  s = -(int32_t)sine_quarter(within); break;
                default: s = -(int32_t)sine_quarter(1024u - within); break;
            }
            return (uint16_t)((s + CV_FULL) >> 1);
        }
    }
}

int16_t cv_shape_scaled(uint16_t raw, uint8_t depth, uint8_t offset_param, bool unipolar){
    const int32_t offset = (int32_t)(int8_t)offset_param * CV_HALF / 128;
    if (unipolar){
        return (int16_t)cv_clamp_unipolar((int32_t)raw * depth / 255 + offset);
    }
    return (int16_t)cv_clamp_bipolar(((int32_t)raw - CV_HALF) * depth / 255 + offset);
}
