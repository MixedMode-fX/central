#ifndef MMMC_ALGORITHM_SEQUENCER_EUCLID_H
#define MMMC_ALGORITHM_SEQUENCER_EUCLID_H

#include <stdint.h>
#include "config.h"

// Bjorklund's algorithm: k pulses spread as evenly as possible over n steps.
//
// Written out the way Bjorklund describes it - k groups of [1] and n-k groups
// of [0], repeatedly folded into each other - rather than by one of the
// several one-line approximations, because those produce *rotations* of the
// published patterns and this module is expected to agree with the reference
// tables: E(3,8) = 10010010, E(5,8) = 10110110, E(7,16) = 1001010100101010.
//
// The pattern is returned as a bitfield, step i in bit i, computed once when
// k, n or the rotation changes - never per edge.

inline uint32_t euclid_mask(uint8_t bits){
    return bits >= 32 ? 0xFFFFFFFFu : (uint32_t)((1u << bits) - 1u);
}

inline uint32_t euclidean_pattern(uint8_t pulses, uint8_t steps){
    if (steps == 0 || steps > MAX_SEQUENCE_LEN) return 0;
    if (pulses == 0) return 0;
    if (pulses >= steps) return euclid_mask(steps);

    // Groups, in their current order, packed into `bits`: the first `n_a` are
    // the ones that started as [1], the rest are the remainders.
    uint32_t bits = euclid_mask(pulses);          // k groups of [1], then zeros
    uint8_t length[MAX_SEQUENCE_LEN];
    uint8_t offset[MAX_SEQUENCE_LEN];
    uint8_t n_groups = steps;
    uint8_t n_a = pulses;
    uint8_t n_b = (uint8_t)(steps - pulses);
    for (uint8_t i = 0; i < n_groups; i++) length[i] = 1;

    while (n_b > 1){
        uint8_t position = 0;
        for (uint8_t i = 0; i < n_groups; i++){ offset[i] = position; position = (uint8_t)(position + length[i]); }

        const uint8_t pairs = n_a < n_b ? n_a : n_b;
        uint32_t merged = 0;
        uint8_t merged_length[MAX_SEQUENCE_LEN];
        uint8_t merged_count = 0;
        uint8_t at = 0;

        // Each of the first `pairs` groups swallows one remainder group.
        for (uint8_t i = 0; i < pairs; i++){
            const uint8_t a = i;
            const uint8_t b = (uint8_t)(n_a + i);
            merged |= ((bits >> offset[a]) & euclid_mask(length[a])) << at;
            at = (uint8_t)(at + length[a]);
            merged |= ((bits >> offset[b]) & euclid_mask(length[b])) << at;
            at = (uint8_t)(at + length[b]);
            merged_length[merged_count++] = (uint8_t)(length[a] + length[b]);
        }
        // Whatever is left over of either kind keeps its own group.
        for (uint8_t i = pairs; i < n_a; i++){
            merged |= ((bits >> offset[i]) & euclid_mask(length[i])) << at;
            at = (uint8_t)(at + length[i]);
            merged_length[merged_count++] = length[i];
        }
        for (uint8_t i = (uint8_t)(n_a + pairs); i < n_groups; i++){
            merged |= ((bits >> offset[i]) & euclid_mask(length[i])) << at;
            at = (uint8_t)(at + length[i]);
            merged_length[merged_count++] = length[i];
        }

        bits = merged;
        for (uint8_t i = 0; i < merged_count; i++) length[i] = merged_length[i];
        n_groups = merged_count;
        n_a = pairs;
        n_b = (uint8_t)(merged_count - pairs);
        if (n_a <= 1) break;
    }
    return bits & euclid_mask(steps);
}

// Starts the pattern `by` steps in, wrapping: rotation is what turns one
// Euclidean rhythm into the family of rhythms that share its shape.
inline uint32_t rotate_pattern(uint32_t pattern, uint8_t steps, uint8_t by){
    if (steps == 0 || steps > MAX_SEQUENCE_LEN) return pattern;
    const uint8_t r = (uint8_t)(by % steps);
    if (r == 0) return pattern & euclid_mask(steps);
    return ((pattern >> r) | (pattern << (steps - r))) & euclid_mask(steps);
}

#endif
