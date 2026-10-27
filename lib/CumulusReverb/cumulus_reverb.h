#ifndef CUMULUS_REVERB_H
#define CUMULUS_REVERB_H

#include <stddef.h>
#include <stdint.h>
#include "audioeffectx.h"

// Cumulus: the reverb of Mutable Instruments' Clouds firmware, by Emilie
// Gillet, as a VST-style plug-in the library's host runs (AudioEffectVST.h).
// The DSP is Clouds' own clouds::Reverb, compiled from the unmodified
// sources under clouds/ and stmlib/ (see clouds/README.md); this class only
// carries its delay memory and turns 0..1 parameters into its settings.
//
// What it is, from the source: the Griesinger topology Dattorro's paper
// describes. Left and right are summed into four input allpasses, then a
// figure-of-eight loop of two allpasses and a delay per side, each side's
// loop feeding one output. A one-pole low-pass sits in each half of the
// loop; one LFO smears the first input allpass and another sways the long
// delay for a slow chorus. The delay lines live in 16384 words of 12-bit
// samples, which is part of how it sounds.
//
// The delay lengths and LFO rates are Clouds' own, tuned for its 32 kHz:
// at 44.1 kHz the space is 0.73 times the size and the shimmer 1.38 times
// as fast as on the module, at 48 kHz 0.67 and 1.5. The sample rate a host
// sets times only the clock's movement, below.
//
// Around it sits a clock of its own, after the variable sample clock of
// delays like Lexicon's Prime Time: the tank runs at a fraction of the
// host's rate, its input held and smoothed down to it and its output
// interpolated back up. A slower clock is a bigger, darker room with an
// aliased edge; moving the clock while the tail rings bends its pitch.
// Warp sweeps the clock down from where Clock sets it, an LFO with a drift
// in it, and Glitch throws it, at random, to a fraction of itself for a
// moment.
//
// The first five parameters follow how Clouds' firmware drives the
// reverb, and with the clock at x1 and still, the defaults are the
// module's sound with its feedback knob at noon:
//
//   0 Amount     wet against dry, as the reverb's own crossfade.
//   1 Time       loop gain, 0.35 to 0.98 as Clouds' reverb knob sets it.
//   2 Diffusion  allpass coefficient, 0.4 to 0.8; Clouds fixes it at 0.7.
//   3 Tone       loop low-pass, 0.12 (murky) to 0.97 (open), on an
//                exponential curve; Clouds' feedback knob moves it from
//                0.6 to 0.97, and at noon sets 0.78, the default here.
//   4 Input      level into the tank, 0 to 0.4; Clouds feeds it 0.2.
//   5 Clock      the tank's rate, x1/8 to x1 of the host's.
//   6 Warp       how far the clock is swept down: to two octaves.
//   7 Speed      the sweep's rate, 0.05 to 12 Hz.
//   8 Glitch     how often the clock jumps, to about three times a second.
class CumulusReverb : public AudioEffectX {
public:
    enum Parameter {
        PARAM_AMOUNT,
        PARAM_TIME,
        PARAM_DIFFUSION,
        PARAM_TONE,
        PARAM_INPUT,
        PARAM_CLOCK,
        PARAM_WARP,
        PARAM_SPEED,
        PARAM_GLITCH,
        NUM_PARAMS
    };

    // What clouds::Reverb needs beside it: its FxEngine's delay memory,
    // FxEngine<16384, FORMAT_12_BIT> in clouds/dsp/fx/reverb.h, and room to
    // construct the object itself, checked against its size where it is
    // compiled (cumulus_reverb.cpp).
    static const size_t DELAY_WORDS = 16384;
    static const size_t REVERB_BYTES = 128;

    explicit CumulusReverb(audioMasterCallback master);
    virtual ~CumulusReverb();
    CumulusReverb(const CumulusReverb&) = delete;
    CumulusReverb& operator=(const CumulusReverb&) = delete;

    virtual void setParameter(VstInt32 index, float value);
    virtual float getParameter(VstInt32 index);
    virtual void getParameterName(VstInt32 index, char* text);
    virtual void getParameterDisplay(VstInt32 index, char* text);
    virtual void getParameterLabel(VstInt32 index, char* text);
    virtual bool getEffectName(char* name);
    virtual bool getVendorString(char* text);
    virtual bool getProductString(char* text);
    virtual VstPlugCategory getPlugCategory() { return kPlugCategRoomFx; }

    virtual void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames);

    // Empties the tank at the start of the next block: the tail stops.
    void clear() { clear_ = true; }

private:
    void rebuild();

    alignas(8) unsigned char storage_[REVERB_BYTES];
    class Engine;
    Engine* engine_;  // a clouds::Reverb, constructed in storage_
    float params_[NUM_PARAMS];
    volatile bool clear_;

    // The clock: how far towards the tank's next sample, 0..1; the tank's
    // last two outputs, interpolated between; its input, smoothed.
    float phase_;
    float last_[2];
    float next_[2];
    float smooth_[2];
    // The clock's movement.
    float warp_phase_;  // 0..1
    float drift_, drift_target_;
    uint32_t drift_left_;
    float glitch_;  // the clock's multiplier while a glitch lasts, else 1
    uint32_t glitch_left_;
    uint32_t noise_;

    uint16_t delay_[DELAY_WORDS];
};

#endif
