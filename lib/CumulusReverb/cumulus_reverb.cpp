#include "cumulus_reverb.h"
#include <new>
#include "mutable_reverb.h"

class CumulusReverb::Engine : public clouds::Reverb {
};

namespace {

const char* const NAMES[CumulusReverb::NUM_PARAMS] = {"Amount", "Time",  "Diffuse", "Tone",  "Input",
                                                      "Clock",  "Warp",  "Speed",   "Glitch"};

// Clouds' fixed diffusion and input level, its feedback knob at noon for
// the tone, and a medium tail a third wet; the clock at x1 and still.
const float DEFAULTS[CumulusReverb::NUM_PARAMS] = {0.3f, 0.5f, 0.75f, 0.75f, 0.5f, 1.0f, 0.0f, 0.3f, 0.0f};

// Tone's loop low-pass coefficient, 1 - e^-c, with c exponential in the
// parameter between these: 0.12 and 0.97. At 0.75 it is Clouds' 0.78.
const float TONE_C_MIN = 0.1278f;
const float TONE_C_MAX = 3.5066f;

// What a glitch multiplies the clock by.
const float GLITCH_STEPS[] = {0.5f, 0.25f, 1.0f / 3.0f, 2.0f / 3.0f, 0.125f, 0.75f};
const int NUM_GLITCH_STEPS = sizeof GLITCH_STEPS / sizeof GLITCH_STEPS[0];

const float PI_F = 3.14159265f;

float clockOf(float value)
{
    return powf(2.0f, -3.0f * (1.0f - value));
}

// sin(2 pi phase) for phase in 0..1, to within 0.001: a parabola, refined.
inline float sine(float phase)
{
    const float t = 2.0f * phase - 1.0f;
    const float y = -4.0f * t + 4.0f * t * fabsf(t);
    return 0.225f * (y * fabsf(y) - y) + y;
}

}  // namespace

CumulusReverb::CumulusReverb(audioMasterCallback master)
    : AudioEffectX(master, 0, NUM_PARAMS),
      storage_(),
      engine_(nullptr),
      params_(),
      clear_(false),
      phase_(0.0f),
      last_(),
      next_(),
      smooth_(),
      warp_phase_(0.0f),
      drift_(0.0f),
      drift_target_(0.0f),
      drift_left_(0),
      glitch_(1.0f),
      glitch_left_(0),
      noise_(0x9e3779b9u),
      delay_()
{
    setNumInputs(2);
    setNumOutputs(2);
    setUniqueID(0x63756d75);  // 'cumu'
    canProcessReplacing();
    for (int i = 0; i < NUM_PARAMS; i++) params_[i] = DEFAULTS[i];
    // clouds::Reverb's constructor leaves its members as they were and
    // Init() sets only some of them: the filter states start from storage_,
    // which is zero, as Clouds' static memory is, and every setting is made
    // at the top of each block.
    engine_ = new (storage_) Engine();
    engine_->Init(delay_);
}

CumulusReverb::~CumulusReverb()
{
    engine_->~Engine();
}

void CumulusReverb::rebuild()
{
    static_assert(sizeof(Engine) <= REVERB_BYTES, "raise REVERB_BYTES");
    static_assert(alignof(Engine) <= 8, "align storage_ for clouds::Reverb");
    // Init() empties the delay memory but not the loop filters' states;
    // building the engine again from zero does both. The clock's samples
    // in flight go with them.
    engine_->~Engine();
    memset(storage_, 0, sizeof storage_);
    engine_ = new (storage_) Engine();
    engine_->Init(delay_);
    phase_ = 0.0f;
    for (int c = 0; c < 2; c++) last_[c] = next_[c] = smooth_[c] = 0.0f;
}

void CumulusReverb::setParameter(VstInt32 index, float value)
{
    if (index < 0 || index >= NUM_PARAMS) return;
    params_[index] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

float CumulusReverb::getParameter(VstInt32 index)
{
    return index >= 0 && index < NUM_PARAMS ? params_[index] : 0.0f;
}

void CumulusReverb::getParameterName(VstInt32 index, char* text)
{
    text[0] = 0;
    if (index >= 0 && index < NUM_PARAMS) vst_strncpy(text, NAMES[index], kVstMaxParamStrLen);
}

void CumulusReverb::getParameterDisplay(VstInt32 index, char* text)
{
    text[0] = 0;
    if (index < 0 || index >= NUM_PARAMS) return;
    const float v = params_[index];
    if (index == PARAM_AMOUNT || index == PARAM_WARP || index == PARAM_GLITCH) {
        int2string(static_cast<VstInt32>(v * 100.0f + 0.5f), text, kVstMaxParamStrLen);
    } else if (index == PARAM_CLOCK) {
        float2string(clockOf(v), text, 5);
    } else if (index == PARAM_SPEED) {
        float2string(0.05f * powf(240.0f, v), text, 5);
    } else {
        float2string(v, text, kVstMaxParamStrLen);
    }
}

void CumulusReverb::getParameterLabel(VstInt32 index, char* text)
{
    const char* label = "";
    if (index == PARAM_AMOUNT || index == PARAM_WARP || index == PARAM_GLITCH) label = "%";
    else if (index == PARAM_CLOCK) label = "x";
    else if (index == PARAM_SPEED) label = "Hz";
    vst_strncpy(text, label, kVstMaxParamStrLen);
}

bool CumulusReverb::getEffectName(char* name)
{
    vst_strncpy(name, "Cumulus", kVstMaxEffectNameLen);
    return true;
}

bool CumulusReverb::getVendorString(char* text)
{
    vst_strncpy(text, "after Emilie Gillet", kVstMaxVendorStrLen);
    return true;
}

bool CumulusReverb::getProductString(char* text)
{
    vst_strncpy(text, "Cumulus reverb", kVstMaxProductStrLen);
    return true;
}

void CumulusReverb::processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames)
{
    if (clear_) {
        clear_ = false;
        rebuild();
    }
    // The parameters are read once a block, as loop() may be moving them.
    // The reverb runs all wet; the crossfade is Clouds' own, done here so
    // the dry signal never goes through the clock.
    const float* p = params_;
    const float rate = getSampleRate();
    engine_->set_amount(1.0f);
    engine_->set_time(0.35f + 0.63f * p[PARAM_TIME]);
    engine_->set_diffusion(0.4f + 0.4f * p[PARAM_DIFFUSION]);
    engine_->set_lp(1.0f - expf(-TONE_C_MIN * powf(TONE_C_MAX / TONE_C_MIN, p[PARAM_TONE])));
    engine_->set_input_gain(0.4f * p[PARAM_INPUT]);
    const float amount = p[PARAM_AMOUNT];

    const float base = clockOf(p[PARAM_CLOCK]);
    const float warp = 2.0f * p[PARAM_WARP] * p[PARAM_WARP];  // octaves
    const float warp_step = 0.05f * powf(240.0f, p[PARAM_SPEED]) / rate;
    const float drift_glide = 1.0f - expf(-2.0f * PI_F * 0.7f / rate);
    const uint32_t drift_hold = static_cast<uint32_t>(0.4f * rate);
    const float glitch_chance = 3.0f * p[PARAM_GLITCH] * p[PARAM_GLITCH] / rate;

    for (VstInt32 i = 0; i < sampleFrames; i++) {
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;

        // The clock: where Clock sets it, swept down by Warp, thrown by
        // Glitch, never slower than x1/64.
        float now = base * glitch_;
        if (warp > 0.0f) {
            warp_phase_ += warp_step;
            if (warp_phase_ >= 1.0f) warp_phase_ -= 1.0f;
            if (drift_left_ == 0) {
                drift_left_ = drift_hold;
                drift_target_ = static_cast<float>(noise_ >> 8) * (1.0f / 16777216.0f);
            }
            drift_left_--;
            drift_ += (drift_target_ - drift_) * drift_glide;
            const float sweep = 0.35f * (1.0f + sine(warp_phase_)) + 0.3f * drift_;  // 0..1
            now *= powf(2.0f, -warp * sweep);
        }
        if (now < 1.0f / 64.0f) now = 1.0f / 64.0f;
        if (glitch_left_ > 0) {
            if (--glitch_left_ == 0) glitch_ = 1.0f;
        } else if (static_cast<float>(noise_ >> 8) * (1.0f / 16777216.0f) < glitch_chance) {
            glitch_ = GLITCH_STEPS[(noise_ >> 3) % NUM_GLITCH_STEPS];
            glitch_left_ = static_cast<uint32_t>((0.03f + 0.37f * static_cast<float>((noise_ >> 11) & 0xff) / 255.0f) * rate) + 1;
        }

        // The tank's input, smoothed towards its rate: at x1 untouched.
        const float smoothing = now * 1.6f < 1.0f ? now * 1.6f : 1.0f;
        for (int c = 0; c < 2; c++) smooth_[c] += (inputs[c][i] - smooth_[c]) * smoothing;

        // A tank sample each time the clock comes round. At x1 that is
        // every host sample, and the output is the tank's own.
        phase_ += now;
        while (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            clouds::FloatFrame frame = {smooth_[0], smooth_[1]};
            engine_->Process(&frame, 1);
            last_[0] = next_[0];
            last_[1] = next_[1];
            next_[0] = frame.l;
            next_[1] = frame.r;
        }
        const float t = now >= 1.0f ? 1.0f : phase_;
        for (int c = 0; c < 2; c++) {
            const float wet = last_[c] + (next_[c] - last_[c]) * t;
            const float dry = inputs[c][i];
            outputs[c][i] = dry + (wet - dry) * amount;
        }
    }
}
