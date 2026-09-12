#ifndef MMMC_MIDI_ROOT_MOTION_H
#define MMMC_MIDI_ROOT_MOTION_H

#include <stdint.h>
#include "midi/scale.h"

// How strongly one chord root wants to move to another, **computed from the
// scale** rather than looked up in a table.
//
// `Harmony` used to carry five 7 x 7 tables of weights, one per named style.
// Tables are honest about what tonal music does and dishonest about
// everything else: they are written for seven degrees, so a pentatonic key
// uses five columns of weights that were tuned for diatonic function and a
// key nobody anticipated gets numbers that mean nothing; they say what a
// genre does rather than why; and the only progressions reachable are the
// ones somebody typed. There is no room in a table for an accident.
//
// So the weights are derived instead, from three facts about the two chords
// that hold in any scale:
//
//  1. **How far the root moved, in semitones.** Not in scale steps: a fifth
//     is seven semitones in every key, and "three degrees up" is a fifth in a
//     major scale and something else in a pentatonic one. Measuring the
//     actual interval is what makes one rule work in a scale it was never
//     written for - and the fifth relation is the strongest in tonal music
//     for reasons of acoustics, not of genre.
//  2. **Which direction round the circle.** A root falling a fifth is the
//     motion that drives tonal music forward; one rising a fifth is its
//     retrograde and is how a dominant is *approached* rather than resolved.
//     One axis, and the two ends are two different kinds of music.
//  3. **How many notes the two chords share.** Triads a third apart share
//     two, a fifth apart share one, a step apart share none. Shared tones are
//     what makes a progression sound smooth rather than blocky, and wanting
//     them is what Romantic mediant harmony is.
//
// Plus one fact about the *destination*: whether its triad carries the
// semitone below the tonic. That note is what an authentic cadence is made of
// and what modal music must avoid, so the same control reads as "how tonal"
// in one direction and "how modal" in the other - and it goes quiet by itself
// in a mode that has no leading tone, because there is then no such triad to
// weight.
//
// Everything here is a pure function of a 12-bit mask and a degree, so it is
// testable without a bus and costs nothing to reason about. `Harmony`
// composes them; nothing here knows what a style is.
namespace root_motion {

    // Degrees a scale can have and this header will describe. Seven is what a
    // triad stacked in thirds means; a scale with more is walked over its
    // first seven, which is Harmony's rule and not this one's.
    static constexpr uint8_t MAX_DEGREES = 7;

    // The pitch classes of the first `n` degrees, relative to the tonic.
    // Written once per decision and read many times, which is the whole
    // reason this is not computed inline: scale_interval walks the mask.
    struct Degrees {
        uint8_t pc[MAX_DEGREES];
        uint8_t n;
    };

    inline Degrees degrees_of(uint16_t mask, uint8_t n){
        Degrees d = {};
        d.n = n > MAX_DEGREES ? MAX_DEGREES : n;
        for (uint8_t i = 0; i < d.n; i++) d.pc[i] = scale_interval(mask, i);
        return d;
    }

    // The triad on a degree, as a set of pitch classes. Stacked in scale
    // steps, so its quality is the key's business and not this header's -
    // the same rule Chord's qualities follow.
    inline uint16_t triad(const Degrees& d, uint8_t degree){
        if (d.n == 0) return 0;
        uint16_t set = 0;
        for (uint8_t k = 0; k < 3; k++){
            set |= (uint16_t)(1u << d.pc[(degree + (uint8_t)(k * 2u)) % d.n]);
        }
        return set;
    }

    // Pitch classes two triads have in common: 0..2 for triads of a seven-note
    // scale, and the measure of how smoothly one can become the other.
    inline uint8_t common_tones(const Degrees& d, uint8_t from, uint8_t to){
        uint16_t both = (uint16_t)(triad(d, from) & triad(d, to));
        uint8_t n = 0;
        while (both){ n = (uint8_t)(n + (both & 1u)); both >>= 1; }
        return n;
    }

    // True when this triad carries the semitone below the tonic - the note an
    // authentic cadence is made of. False for every triad of a mode that does
    // not have one, which is what makes the control that reads this go quiet
    // by itself in Dorian or Mixolydian.
    inline bool carries_leading_tone(const Degrees& d, uint8_t degree){
        return (triad(d, degree) & (uint16_t)(1u << 11)) != 0;
    }

    // Semitones from one root up to another, 0..11.
    inline uint8_t interval(const Degrees& d, uint8_t from, uint8_t to){
        if (from >= d.n || to >= d.n) return 0;
        return (uint8_t)(((int16_t)d.pc[to] - (int16_t)d.pc[from] + 12) % 12);
    }

    // How strong a root motion of this many semitones is, before any
    // preference is applied. The fifth relations lead, the thirds follow, the
    // steps are ordinary and the tritone is the rare one - which is what
    // makes it the interesting one when `spread` lets it through.
    inline uint8_t strength(uint8_t semitones){
        switch (semitones % 12u){
            case 5: case 7:  return 12;    // a fifth, either way round
            case 3: case 4:
            case 8: case 9:  return 4;     // a third, either way round
            case 2: case 10: return 5;     // a whole step
            case 1: case 11: return 3;     // a semitone
            case 6:          return 1;     // the tritone
            default:         return 0;     // a repeat: `pull` decides those
        }
    }

    // How strong a move is when what matters is not how far the root went but
    // how much the two chords share. The counterpart to strength(), on the
    // same scale, so one can be crossfaded into the other: at one end the
    // fifth relation leads, at the other the mediant does, and those are two
    // different theories of what makes a progression.
    inline uint8_t shared_strength(uint8_t shared){
        switch (shared){
            case 0:  return 1;     // nothing in common: a step, and a jolt
            case 1:  return 6;     // a fifth apart
            default: return 14;    // a third apart, and the smoothest there is
        }
    }

    // Which way round the circle of fifths this motion goes. A root falling a
    // fifth or rising a third drives forward; one rising a fifth or falling a
    // third is the retrograde. Everything else is neither.
    enum Direction : uint8_t { MOTION_NEUTRAL = 0, MOTION_FALLING = 1, MOTION_RISING = 2 };

    inline Direction direction(uint8_t semitones){
        switch (semitones % 12u){
            case 5: case 3: case 4: return MOTION_FALLING;   // down a fifth, up a third
            case 7: case 8: case 9: return MOTION_RISING;    // up a fifth, down a third
            default:                return MOTION_NEUTRAL;
        }
    }
}

#endif
