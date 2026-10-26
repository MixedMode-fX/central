#ifndef VST_HOST_EFFECT_H
#define VST_HOST_EFFECT_H

#include <string.h>
#include "audioeffectx.h"
#include "vst_host/sample_format.h"

// Channels a host carries through a plug-in: stereo, which is what every
// Airwindows plug-in is. Overridable from the build.
#ifndef VST_HOST_MAX_CHANNELS
#define VST_HOST_MAX_CHANNELS 2
#endif

// One plug-in and what a host does with it that has nothing to do with the
// Audio Library: its parameters and their text, bypass, the sample rate it
// is told, and processing a block of float channels. AudioEffectVSTHost puts
// the Audio Library around one of these; AudioEffectVSTChain holds a row of
// them. Framework-free.
//
// Parameters are 0..1 floats, set from loop() and read by the plug-in at the
// start of each block it processes; a float store is one instruction, so no
// locking is needed. A plug-in declaring more channels than MAX_CHANNELS is
// passed through unprocessed and says so in supported(). An effect with no
// plug-in is a wire.
//
// Two controls of the host's own sit after the plug-in: mix() blends the
// plug-in's output with its input (1 is all plug-in, 0 all input), and
// gain() scales the result, which is the effect's weight when a chain sums
// it with others in parallel.
class VSTEffect {
public:
    static const int MAX_CHANNELS = VST_HOST_MAX_CHANNELS;
    // Room for every string a plug-in writes, with its terminator; plug-ins
    // have been seen to write a product-length string as their name.
    static const int TEXT_LEN = kVstMaxProductStrLen + 1;

    VSTEffect() : plugin_(nullptr), bypass_(false), mix_(1.0f), gain_(1.0f) {}
    explicit VSTEffect(AudioEffectX& plugin) : plugin_(&plugin), bypass_(false), mix_(1.0f), gain_(1.0f) {}
    VSTEffect(const VSTEffect&) = delete;
    VSTEffect& operator=(const VSTEffect&) = delete;

    void attach(AudioEffectX& plugin) { plugin_ = &plugin; }
    bool empty() const { return plugin_ == nullptr; }
    AudioEffectX& plugin() { return *plugin_; }  // only when not empty()
    bool supported() const
    {
        return plugin_ && plugin_->getNumInputs() <= MAX_CHANNELS && plugin_->getNumOutputs() <= MAX_CHANNELS;
    }

    void sampleRate(float hz)
    {
        if (plugin_) plugin_->setSampleRate(hz);
    }
    float sampleRate() const { return plugin_ ? plugin_->getSampleRate() : 0.0f; }

    void bypass(bool on) { bypass_ = on; }
    bool bypassed() const { return bypass_; }

    void mix(float wet) { mix_ = wet < 0.0f ? 0.0f : (wet > 1.0f ? 1.0f : wet); }
    float mix() const { return mix_; }
    void gain(float level) { gain_ = level < -MAX_GAIN ? -MAX_GAIN : (level > MAX_GAIN ? MAX_GAIN : level); }
    float gain() const { return gain_; }

    int numParams() const { return plugin_ ? plugin_->getNumParams() : 0; }
    void setParameter(int index, float value)
    {
        if (index < 0 || index >= numParams()) return;
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        plugin_->setParameter(index, value);
    }
    float getParameter(int index) { return index >= 0 && index < numParams() ? plugin_->getParameter(index) : 0.0f; }
    void parameterName(int index, char* text)  // text[TEXT_LEN]
    {
        text[0] = 0;
        if (index >= 0 && index < numParams()) plugin_->getParameterName(index, text);
    }
    void parameterDisplay(int index, char* text)  // the value as the plug-in shows it
    {
        text[0] = 0;
        if (index >= 0 && index < numParams()) plugin_->getParameterDisplay(index, text);
    }
    void parameterLabel(int index, char* text)  // its unit
    {
        text[0] = 0;
        if (index >= 0 && index < numParams()) plugin_->getParameterLabel(index, text);
    }
    void effectName(char* text)
    {
        text[0] = 0;
        if (plugin_) plugin_->getEffectName(text);
        else vst_strncpy(text, "wire", kVstMaxEffectNameLen);
    }

    // Runs the plug-in over MAX_CHANNELS buffers of frames each, in to out,
    // which must be distinct, then applies mix() and gain(). Bypassed,
    // unsupported or a wire, the plug-in's output is its input; a plug-in
    // with fewer outputs leaves silence in the rest.
    void process(float** in, float** out, int frames)
    {
        const float wet = mix_;
        const float level = gain_;
        if (bypass_ || !supported()) {
            for (int ch = 0; ch < MAX_CHANNELS; ch++) scale(in[ch], out[ch], level, frames);
            return;
        }
        plugin_->processReplacing(in, out, frames);
        for (int ch = plugin_->getNumOutputs(); ch < MAX_CHANNELS; ch++) vst_host::silence(out[ch], frames);
        if (wet == 1.0f && level == 1.0f) return;
        const float dry = 1.0f - wet;
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            for (int i = 0; i < frames; i++) out[ch][i] = level * (wet * out[ch][i] + dry * in[ch][i]);
        }
    }

private:
    static const int MAX_GAIN = 8;

    static void scale(const float* in, float* out, float level, int frames)
    {
        if (level == 1.0f) {
            memcpy(out, in, frames * sizeof(float));
            return;
        }
        for (int i = 0; i < frames; i++) out[i] = level * in[i];
    }

    AudioEffectX* plugin_;
    volatile bool bypass_;
    volatile float mix_;
    volatile float gain_;
};

#endif
