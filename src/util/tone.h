#ifndef MMMC_UTIL_TONE_H
#define MMMC_UTIL_TONE_H

#include <stdint.h>

// What a number on a CV bus means to a tone jack (GATE_PORT_TONE,
// node/patch.h): a pitch, as a MIDI note in 1/TONE_PER_SEMITONE steps, which
// the jack plays as a square wave.
//
// **The scale is the MIDI note number, not a range and a base.** A tone jack
// drives something that listens for a frequency - a pitch-to-control input
// such as the BOSS RPS-10's keyboard control - so the number it reads has to
// name one frequency with nothing to configure. Thirty-two steps a semitone
// puts note 127 at 4064, inside the unipolar range, and a step at about
// three cents, finer than any pitch tracker it feeds.
//
// Zero and below is silence: note zero is 8 Hz, which nothing tracks, and a
// writer that stops writing leaves its bus at zero after the swap, so a
// jack whose source goes away goes quiet rather than holding a pitch.

static constexpr int16_t TONE_PER_SEMITONE = 32;

inline int16_t tone_of_note(uint8_t note){
    return (int16_t)((int16_t)note * TONE_PER_SEMITONE);
}

// The frequency of a tone pitch, in millihertz; 0 for silence.
inline uint32_t tone_millihertz(int16_t pitch){
    if (pitch <= 0) return 0;
    // 2^(s/12) and 2^(k/384), Q16: a semitone table and a step table, so
    // the arithmetic is two multiplies and a shift with no libm.
    static constexpr uint32_t SEMI[12] = {
        65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
    };
    static constexpr uint32_t STEP[TONE_PER_SEMITONE] = {
        65536, 65654, 65773, 65892, 66011, 66130, 66250, 66369, 66489, 66609, 66730, 66850,
        66971, 67092, 67213, 67335, 67456, 67578, 67700, 67823, 67945, 68068, 68191, 68314,
        68438, 68561, 68685, 68809, 68933, 69058, 69183, 69308,
    };
    static constexpr uint64_t NOTE_ZERO_UHZ = 8175799;   // 440 Hz * 2^(-69/12)
    const uint16_t p = (uint16_t)pitch;
    const uint16_t octave = (uint16_t)(p / (12u * TONE_PER_SEMITONE));
    const uint16_t within = (uint16_t)(p % (12u * TONE_PER_SEMITONE));
    uint64_t uhz = (NOTE_ZERO_UHZ * SEMI[within / TONE_PER_SEMITONE]) >> 16;
    uhz = (uhz * STEP[within % TONE_PER_SEMITONE]) >> 16;
    uhz <<= octave;
    return (uint32_t)((uhz + 500u) / 1000u);
}

#endif
