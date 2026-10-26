#ifndef __audioeffectx__
#define __audioeffectx__
#define __audioeffect__

// A stand-in for the VST 2.x SDK's audioeffectx.h: the plug-in side of the
// interface (AudioEffect, AudioEffectX, their constants and string helpers),
// with no host, no editor and none of Steinberg's code. Effect plug-ins
// written against that interface compile unmodified on top of it, and
// AudioEffectVST (AudioEffectVST.h) runs them as Audio Library objects.
//
// The interface was written down from how the open-source Airwindows
// plug-ins (github.com/airwindows/airwindows, MIT) use it: construction with
// an audioMasterCallback, the setNumInputs/setUniqueID/canProcessReplacing
// sequence, getSampleRate() read at the top of every block, parameters as
// 0..1 floats with vst_strncpy'd names and float2string'd displays, and
// processReplacing on float buffers. Anything else of the SDK is a sensible
// no-op or default. A host-side opcode interface does not exist: a plug-in is
// driven by calling its methods, which is all a microcontroller needs.
//
// Two things make unmodified plug-in sources possible and are worth knowing:
//
// - The standard headers a plug-in includes after this one (<set>, <string>,
//   <math.h>, ...) are included here first, so that a plug-in's sources can
//   be wrapped in a namespace of their own, as examples/VSTHost/plugins.h
//   does: every Airwindows plug-in names its parameters kParamA.. and its
//   factory createEffectInstance at file scope, and two of them can only
//   share a sketch with that wrapping.
//
// - `default: throw;` is how the Airwindows setParameter() ends, and GCC
//   refuses a throw expression outright when exceptions are off, as they are
//   in every Teensy build. The keyword is defined away below in that case
//   only. Nothing the core compiles can contain one either, so no other code
//   is affected.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

typedef int16_t VstInt16;
typedef int32_t VstInt32;
typedef int64_t VstInt64;
typedef intptr_t VstIntPtr;

enum VstStringConstants {
    kVstMaxProgNameLen = 24,
    kVstMaxParamStrLen = 8,
    kVstMaxVendorStrLen = 64,
    kVstMaxProductStrLen = 64,
    kVstMaxEffectNameLen = 32,
    kVstMaxLabelLen = 64,
    kVstMaxShortLabelLen = 8
};

enum VstPlugCategory {
    kPlugCategUnknown = 0,
    kPlugCategEffect,
    kPlugCategSynth,
    kPlugCategAnalysis,
    kPlugCategMastering,
    kPlugCategSpacializer,
    kPlugCategRoomFx,
    kPlugSurroundFx,
    kPlugCategRestoration,
    kPlugCategOfflineProcess,
    kPlugCategShell,
    kPlugCategGenerator,
    kPlugCategMaxCount
};

enum VstPinPropertiesFlags { kVstPinIsActive = 1, kVstPinIsStereo = 2, kVstPinUseSpeaker = 4 };

struct VstPinProperties {
    char label[kVstMaxLabelLen];
    VstInt32 flags;
    VstInt32 arrangementType;
    char shortLabel[kVstMaxShortLabelLen];
    char future[48];
};

class AudioEffect;

// What a plug-in calls back into the host with. There is no host here; the
// plug-ins are constructed with a null callback and never call it.
typedef VstIntPtr (*audioMasterCallback)(AudioEffect* effect, VstInt32 opcode, VstInt32 index,
                                         VstIntPtr value, void* ptr, float opt);

// Copies at most maxLen characters and always terminates: the destination
// must hold maxLen + 1, as every buffer written to by the SDK's conventions
// does.
inline char* vst_strncpy(char* dst, const char* src, size_t maxLen)
{
    size_t n = 0;
    while (n < maxLen && src[n] != 0) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return dst;
}

inline char* vst_strncat(char* dst, const char* src, size_t maxLen)
{
    const size_t used = strlen(dst);
    if (used < maxLen) vst_strncpy(dst + used, src, maxLen - used);
    return dst;
}

class AudioEffect {
public:
    AudioEffect(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
        : audioMaster_(audioMaster), numPrograms_(numPrograms), numParams_(numParams), numInputs_(2),
          numOutputs_(2), uniqueID_(0), initialDelay_(0), sampleRate_(44100.0f), blockSize_(128),
          curProgram_(0)
    {
    }
    virtual ~AudioEffect() {}

    // Parameters: 0..1 floats, text in buffers of kVstMaxParamStrLen + 1.
    virtual void setParameter(VstInt32 index, float value) { (void)index, (void)value; }
    virtual float getParameter(VstInt32 index) { (void)index; return 0.0f; }
    virtual void getParameterLabel(VstInt32 index, char* label) { (void)index; label[0] = 0; }
    virtual void getParameterDisplay(VstInt32 index, char* text) { (void)index; text[0] = 0; }
    virtual void getParameterName(VstInt32 index, char* text) { (void)index; text[0] = 0; }

    // Programs (presets), which the host does not use.
    virtual VstInt32 getProgram() { return curProgram_; }
    virtual void setProgram(VstInt32 program) { curProgram_ = program; }
    virtual void setProgramName(char* name) { (void)name; }
    virtual void getProgramName(char* name) { name[0] = 0; }
    virtual VstInt32 getChunk(void** data, bool isPreset = false) { (void)data, (void)isPreset; return 0; }
    virtual VstInt32 setChunk(void* data, VstInt32 byteSize, bool isPreset = false)
    {
        (void)data, (void)byteSize, (void)isPreset;
        return 0;
    }

    // Transport, from the host's side.
    virtual void setSampleRate(float sampleRate) { sampleRate_ = sampleRate; }
    virtual void setBlockSize(VstInt32 blockSize) { blockSize_ = blockSize; }
    virtual void suspend() {}
    virtual void resume() {}
    virtual void open() {}
    virtual void close() {}

    // Processing. The buffers are one per channel, getNumInputs() in and
    // getNumOutputs() out, sampleFrames long.
    virtual void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames) = 0;
    virtual void processDoubleReplacing(double** inputs, double** outputs, VstInt32 sampleFrames)
    {
        (void)inputs, (void)outputs, (void)sampleFrames;
    }

    float getSampleRate() const { return sampleRate_; }
    VstInt32 getBlockSize() const { return blockSize_; }
    VstInt32 getNumInputs() const { return numInputs_; }
    VstInt32 getNumOutputs() const { return numOutputs_; }
    VstInt32 getNumParams() const { return numParams_; }
    VstInt32 getNumPrograms() const { return numPrograms_; }
    VstInt32 getUniqueID() const { return uniqueID_; }
    VstInt32 getInitialDelay() const { return initialDelay_; }

    // What a plug-in declares about itself from its constructor.
    void setNumInputs(VstInt32 inputs) { numInputs_ = inputs; }
    void setNumOutputs(VstInt32 outputs) { numOutputs_ = outputs; }
    void setUniqueID(VstInt32 id) { uniqueID_ = id; }
    void setInitialDelay(VstInt32 delay) { initialDelay_ = delay; }
    void canProcessReplacing(bool state = true) { (void)state; }
    void canDoubleReplacing(bool state = true) { (void)state; }
    void programsAreChunks(bool state = true) { (void)state; }
    void setEditor(void* editor) { (void)editor; }

    // Value-to-text helpers, as the SDK names them. Each writes at most
    // maxLen characters plus the terminator.
    static void float2string(float value, char* text, VstInt32 maxLen)
    {
        // As many decimals as fit in maxLen characters.
        int digits = 1;
        float magnitude = fabsf(value);
        while (magnitude >= 10.0f && digits < 12) {
            magnitude /= 10.0f;
            digits++;
        }
        int decimals = maxLen - digits - 1 - (value < 0.0f ? 1 : 0);
        if (decimals < 0) decimals = 0;
        char buffer[32];
        snprintf(buffer, sizeof buffer, "%.*f", decimals, static_cast<double>(value));
        vst_strncpy(text, buffer, static_cast<size_t>(maxLen));
    }
    static void int2string(VstInt32 value, char* text, VstInt32 maxLen)
    {
        char buffer[16];
        snprintf(buffer, sizeof buffer, "%ld", static_cast<long>(value));
        vst_strncpy(text, buffer, static_cast<size_t>(maxLen));
    }
    static void dB2string(float value, char* text, VstInt32 maxLen)
    {
        if (value <= 0.0f) {
            vst_strncpy(text, "-oo", static_cast<size_t>(maxLen));
            return;
        }
        float2string(20.0f * log10f(value), text, maxLen);
    }
    static void Hz2string(float samples, char* text, VstInt32 maxLen)
    {
        if (samples <= 0.0f) {
            vst_strncpy(text, "0", static_cast<size_t>(maxLen));
            return;
        }
        float2string(44100.0f / samples, text, maxLen);
    }
    static void ms2string(float samples, char* text, VstInt32 maxLen)
    {
        float2string(samples * 1000.0f / 44100.0f, text, maxLen);
    }

protected:
    audioMasterCallback audioMaster_;
    VstInt32 numPrograms_;
    VstInt32 numParams_;
    VstInt32 numInputs_;
    VstInt32 numOutputs_;
    VstInt32 uniqueID_;
    VstInt32 initialDelay_;
    float sampleRate_;
    VstInt32 blockSize_;
    VstInt32 curProgram_;
};

class AudioEffectX : public AudioEffect {
public:
    AudioEffectX(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
        : AudioEffect(audioMaster, numPrograms, numParams)
    {
    }

    // Identification. Buffers: kVstMaxEffectNameLen + 1 for the name, though
    // plug-ins have been seen to write kVstMaxProductStrLen into it, so a
    // host gives every one kVstMaxProductStrLen + 1.
    virtual bool getEffectName(char* name) { name[0] = 0; return false; }
    virtual bool getVendorString(char* text) { text[0] = 0; return false; }
    virtual bool getProductString(char* text) { text[0] = 0; return false; }
    virtual VstInt32 getVendorVersion() { return 0; }
    virtual VstPlugCategory getPlugCategory() { return kPlugCategUnknown; }
    virtual VstInt32 canDo(char* text) { (void)text; return 0; }
    virtual VstInt32 getVstVersion() { return 2400; }
    virtual VstInt32 getTailSize() { return 0; }
    virtual bool setBypass(bool onOff) { (void)onOff; return false; }
    virtual bool getInputProperties(VstInt32 index, VstPinProperties* properties)
    {
        (void)index, (void)properties;
        return false;
    }
    virtual bool getOutputProperties(VstInt32 index, VstPinProperties* properties)
    {
        (void)index, (void)properties;
        return false;
    }
    virtual bool string2parameter(VstInt32 index, char* text) { (void)index, (void)text; return false; }
    virtual bool canParameterBeAutomated(VstInt32 index) { (void)index; return true; }

    // Host services a plug-in may ask for. There is no host.
    void isSynth(bool state = true) { (void)state; }
    void noTail(bool state = true) { (void)state; }
    bool beginEdit(VstInt32 index) { (void)index; return false; }
    bool endEdit(VstInt32 index) { (void)index; return false; }
    void setParameterAutomated(VstInt32 index, float value) { setParameter(index, value); }
    bool updateDisplay() { return false; }
    bool sizeWindow(VstInt32 width, VstInt32 height) { (void)width, (void)height; return false; }
    VstInt32 canHostDo(char* text) { (void)text; return 0; }
    bool getHostVendorString(char* text) { text[0] = 0; return false; }
    bool getHostProductString(char* text) { text[0] = 0; return false; }
    VstInt32 getHostVendorVersion() { return 0; }
    VstInt32 getMasterVersion() { return 2400; }
    VstInt32 getCurrentUniqueId() { return getUniqueID(); }
    VstInt32 getCurrentProcessLevel() { return 0; }
    VstInt32 getAutomationState() { return 0; }
};

#if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS)
#define throw
#endif

#endif
