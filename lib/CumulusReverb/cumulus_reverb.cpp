#include "cumulus_reverb.h"
#include <new>
#include "mutable_reverb.h"

class CumulusReverb::Engine : public clouds::Reverb {
};

namespace {

const char* const NAMES[CumulusReverb::NUM_PARAMS] = {"Amount", "Time", "Diffuse", "Tone", "Input"};

// Clouds' fixed diffusion and input level, its feedback knob at noon for
// the tone, and a medium tail a third wet.
const float DEFAULTS[CumulusReverb::NUM_PARAMS] = {0.3f, 0.5f, 0.75f, 0.5f, 0.5f};

}  // namespace

CumulusReverb::CumulusReverb(audioMasterCallback master)
    : AudioEffectX(master, 0, NUM_PARAMS), storage_(), engine_(nullptr), params_(), clear_(false), delay_()
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
    // building the engine again from zero does both.
    engine_->~Engine();
    memset(storage_, 0, sizeof storage_);
    engine_ = new (storage_) Engine();
    engine_->Init(delay_);
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
    if (index == PARAM_AMOUNT) int2string(static_cast<VstInt32>(params_[PARAM_AMOUNT] * 100.0f + 0.5f), text, kVstMaxParamStrLen);
    else if (index >= 0 && index < NUM_PARAMS) float2string(params_[index], text, kVstMaxParamStrLen);
}

void CumulusReverb::getParameterLabel(VstInt32 index, char* text)
{
    vst_strncpy(text, index == PARAM_AMOUNT ? "%" : "", kVstMaxParamStrLen);
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
    engine_->set_amount(params_[PARAM_AMOUNT]);
    engine_->set_time(0.35f + 0.63f * params_[PARAM_TIME]);
    engine_->set_diffusion(0.4f + 0.4f * params_[PARAM_DIFFUSION]);
    engine_->set_lp(0.6f + 0.37f * params_[PARAM_TONE]);
    engine_->set_input_gain(0.4f * params_[PARAM_INPUT]);

    clouds::FloatFrame run[RUN_FRAMES];
    for (VstInt32 done = 0; done < sampleFrames; done += RUN_FRAMES) {
        const int n = sampleFrames - done < RUN_FRAMES ? static_cast<int>(sampleFrames - done) : RUN_FRAMES;
        for (int i = 0; i < n; i++) {
            run[i].l = inputs[0][done + i];
            run[i].r = inputs[1][done + i];
        }
        engine_->Process(run, static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            outputs[0][done + i] = run[i].l;
            outputs[1][done + i] = run[i].r;
        }
    }
}
