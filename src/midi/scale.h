#ifndef MMMC_MIDI_SCALE_H
#define MMMC_MIDI_SCALE_H

#include <stdint.h>

// Scales as 12-bit masks, bit 0 being the root (#10, shared with #13).
//
// One representation for the whole module: Quantise snaps arbitrary pitches
// to it, and the note sequencers pick degrees out of it. A mask is cheap to
// store in a preset and cheap to test - `mask & (1 << interval)`.
enum ScaleId : uint8_t {
    SCALE_CHROMATIC = 0,
    SCALE_MAJOR,
    SCALE_NATURAL_MINOR,
    SCALE_HARMONIC_MINOR,
    SCALE_MELODIC_MINOR,
    SCALE_PENTATONIC_MAJOR,
    SCALE_PENTATONIC_MINOR,
    SCALE_BLUES,
    SCALE_DORIAN,
    SCALE_PHRYGIAN,
    SCALE_LYDIAN,
    SCALE_MIXOLYDIAN,
    SCALE_LOCRIAN,
    SCALE_WHOLE_TONE,
    SCALE_COUNT,
};

// Semitones from the root, as bits.
inline uint16_t scale_mask(uint8_t id){
    switch (id){
        case SCALE_MAJOR:             return 0b101010110101;   // 0 2 4 5 7 9 11
        case SCALE_NATURAL_MINOR:     return 0b010110101101;   // 0 2 3 5 7 8 10
        case SCALE_HARMONIC_MINOR:    return 0b100110101101;   // 0 2 3 5 7 8 11
        case SCALE_MELODIC_MINOR:     return 0b101010101101;   // 0 2 3 5 7 9 11
        case SCALE_PENTATONIC_MAJOR:  return 0b001010010101;   // 0 2 4 7 9
        case SCALE_PENTATONIC_MINOR:  return 0b010010101001;   // 0 3 5 7 10
        case SCALE_BLUES:             return 0b010011101001;   // 0 3 5 6 7 10
        case SCALE_DORIAN:            return 0b011010101101;   // 0 2 3 5 7 9 10
        case SCALE_PHRYGIAN:          return 0b010110101011;   // 0 1 3 5 7 8 10
        case SCALE_LYDIAN:            return 0b101011010101;   // 0 2 4 6 7 9 11
        case SCALE_MIXOLYDIAN:        return 0b011010110101;   // 0 2 4 5 7 9 10
        case SCALE_LOCRIAN:           return 0b010101101011;   // 0 1 3 5 6 8 10
        case SCALE_WHOLE_TONE:        return 0b010101010101;   // 0 2 4 6 8 10
        default:                      return 0b111111111111;   // chromatic
    }
}

// Snaps `note` to the nearest pitch in the scale `mask` rooted at pitch class
// `root` (0..11). Searches outward from the note itself, so the result is
// never more than six semitones away; a tie goes up, which keeps a rising
// line rising. Returns the note unchanged if the mask is empty, and never
// leaves 0..127.
inline uint8_t scale_quantise(uint8_t note, uint8_t root, uint16_t mask){
    mask &= 0x0FFF;
    if (mask == 0 || mask == 0x0FFF) return note;
    const uint8_t r = (uint8_t)(root % 12u);
    for (uint8_t distance = 0; distance <= 6; distance++){
        const int16_t up = (int16_t)note + distance;
        if (up <= 127){
            const uint8_t degree = (uint8_t)(((up - r) % 12 + 12) % 12);
            if (mask & (uint16_t)(1u << degree)) return (uint8_t)up;
        }
        if (distance == 0) continue;
        const int16_t down = (int16_t)note - distance;
        if (down >= 0){
            const uint8_t degree = (uint8_t)(((down - r) % 12 + 12) % 12);
            if (mask & (uint16_t)(1u << degree)) return (uint8_t)down;
        }
    }
    return note;
}

#endif
