#ifndef FERRIC_TAPE_H
#define FERRIC_TAPE_H

#include <stddef.h>
#include <stdint.h>
#include "audioeffectx.h"

// The slap's memory per side, in 12-bit words, a power of two: 350 ms at
// 44.1 kHz, or 185 ms on a Teensy 3.6, whose 256 KB it would otherwise all
// but fill. Overridable from the build.
#ifndef FERRIC_SLAP_WORDS
#if defined(__MK66FX1M0__)
#define FERRIC_SLAP_WORDS 8192
#else
#define FERRIC_SLAP_WORDS 16384
#endif
#endif

// Ferric: a worn tape machine with a slap-back head, as a VST-style plug-in
// the library's host runs (AudioEffectVST.h). Stereo in, stereo out.
//
// The signal, in order:
//
//   compressor   feed-forward, stereo-linked, threshold -12 dBFS, a 10 ms
//                attack and 150 ms release, with half its gain made up.
//   drive        gain into an arctan curve normalised so full scale stays
//                full scale: more drive is more density, not more level.
//   bits         rounding to 16 bits down to 4.
//   tape         a delay line the transport's speed wobbles: wow, slow and
//                drifting, and flutter, fast and scraped with noise. The
//                chorus is two more heads on the same tape, one per side,
//                swept in quadrature, so they wobble with it.
//   dropouts     at random, the oxide thins: level dips for 20 to 200 ms
//                and the top end goes with it.
//   tone         a 30 Hz high-pass and two one-pole low-passes.
//   slap         an echo off the same transport, its repeats darkened and
//                saturated in the loop.
//
// Both delay lines store 12-bit samples in 16-bit words, as the delay
// memory of Mutable Instruments' Clouds does (its FxEngine's FORMAT_12_BIT):
// a step of 1/4096 and headroom to 8. What goes round the echo loop picks
// up that grain on every pass, which is the darkness CumulusReverb has.
//
// The times follow the sample rate a host sets. The parameters:
//
//    0 Drive     0 to 24 dB into the curve.
//    1 Comp      ratio 1:1 to 6:1.
//    2 Bits      16 down to 4.
//    3 Tone      the low-pass, 1.2 to 16 kHz.
//    4 Wow       up to 5 ms of slow pitch drift.
//    5 Flutter   up to 0.3 ms of fast pitch shake.
//    6 Dropout   how often the tape drops out, and how far.
//    7 Chorus    the depth and level of the extra heads.
//    8 Time      the slap's delay, 40 to 350 ms (185 on a Teensy 3.6).
//    9 Feedback  its repeats; at the top they hold, the curve bounding them.
//   10 Slap      its level.
//   11 Mix       the tape against the dry input.
class FerricTape : public AudioEffectX {
public:
    enum Parameter {
        PARAM_DRIVE,
        PARAM_COMP,
        PARAM_BITS,
        PARAM_TONE,
        PARAM_WOW,
        PARAM_FLUTTER,
        PARAM_DROPOUT,
        PARAM_CHORUS,
        PARAM_TIME,
        PARAM_FEEDBACK,
        PARAM_SLAP,
        PARAM_MIX,
        NUM_PARAMS
    };

    // Each side's delay memory, in 12-bit words, powers of two. The tape
    // heads reach 24 ms back, which TAPE_WORDS holds up to 85 kHz; above
    // that the chorus head stops at the end of the tape. The slap's longest
    // time is what SLAP_WORDS holds at the rate set, to 350 ms.
    static const size_t TAPE_WORDS = 2048;
    static const size_t SLAP_WORDS = FERRIC_SLAP_WORDS;

    explicit FerricTape(audioMasterCallback master);
    FerricTape(const FerricTape&) = delete;
    FerricTape& operator=(const FerricTape&) = delete;

    virtual void setParameter(VstInt32 index, float value);
    virtual float getParameter(VstInt32 index);
    virtual void getParameterName(VstInt32 index, char* text);
    virtual void getParameterDisplay(VstInt32 index, char* text);
    virtual void getParameterLabel(VstInt32 index, char* text);
    virtual bool getEffectName(char* name);
    virtual bool getVendorString(char* text);
    virtual bool getProductString(char* text);
    virtual VstPlugCategory getPlugCategory() { return kPlugCategEffect; }

    virtual void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames);

    // Erases the tape and the echoes at the start of the next block.
    void clear() { clear_ = true; }

private:
    struct Channel {
        float hp;        // the high-pass's low-pass state
        float lp1, lp2;  // the tone low-passes
        float fb_lp;     // the slap loop's low-pass
        float fb_hp;     // and its high-pass's state
    };

    void reset();
    float slapMs(float value) const;

    float params_[NUM_PARAMS];
    volatile bool clear_;

    uint32_t write_;  // both lines' write position, masked to each one's size
    uint32_t noise_;

    float wow_phase_, flutter_phase_, chorus_phase_;  // 0..1
    float drift_, drift_mid_, drift_target_;  // wow's random part, -1..1
    uint32_t drift_left_;
    float scrape_;  // flutter's noise
    float dropout_gain_, dropout_target_;
    uint32_t dropout_left_;
    float envelope_;    // the compressor's
    float slap_delay_;  // in samples, gliding to the Time set

    Channel ch_[2];
    uint16_t tape_[2][TAPE_WORDS];
    uint16_t slap_[2][SLAP_WORDS];
};

#endif
