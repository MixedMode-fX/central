#ifndef VST_HOST_AUDIO_EFFECT_VST_H
#define VST_HOST_AUDIO_EFFECT_VST_H

#include <Arduino.h>
#include <AudioStream.h>
#include "audioeffectx.h"
#include "vst_host/effect.h"
#include "vst_host/rack.h"

// Effects written to the VST 2.x plug-in interface (the AudioEffectX class
// of audioeffectx.h) as Teensy Audio Library objects. Inputs 0 and 1 go to
// a plug-in's two inputs, its two outputs come out as outputs 0 and 1, a
// block at a time, in float.
//
// One plug-in, owned by the object:
//
//     AudioEffectVST<airwindows::capacitor::Capacitor> filter;
//     AudioConnection c1(in, 0, filter, 0);
//     filter.setParameter(0, 0.5f);                    // 0..1, as VST does
//
// A rack of them in one object, which keeps the audio in float from one
// plug-in to the next instead of going through the Audio Library's 16-bit
// blocks between every two, in series, in parallel, or both:
//
//     VSTPlugin<airwindows::capacitor::Capacitor> capacitor;
//     VSTPlugin<airwindows::pressure4::Pressure4> pressure;
//     AudioEffectVSTChain chain;
//     ...
//     chain.add(capacitor);                            // in setup(), in signal order
//     chain.add(pressure)->gain(0.7f);                 // the compressed signal...
//     chain.addParallel()->gain(0.5f);                 // ...summed with the dry one
//     chain.slot(1)->setParameter(0, 0.3f);
//
// The control surface of a plug-in in either (parameters, bypass, mix,
// gain, names) is VSTEffect (vst_host/effect.h): AudioEffectVSTHost is one,
// and the chain's slots are. The chain's routing is VSTRack
// (vst_host/rack.h). Every plug-in is told AUDIO_SAMPLE_RATE_EXACT to begin
// with; tell it the interface's actual rate with sampleRate() once known.

// A plug-in instance a sketch can declare without the SDK's constructor
// argument: VSTPlugin<P> is a P constructed with no host callback.
template <class Plugin>
class VSTPlugin : public Plugin {
public:
    VSTPlugin() : Plugin(nullptr) {}
};

// One plug-in as an Audio Library object. An unconnected input is silence
// to the plug-in, which still runs, so a reverb's tail rings out after its
// source stops. Bypassed, the input blocks pass through untouched and the
// plug-in does not run.
class AudioEffectVSTHost : public AudioStream, public VSTEffect {
public:
    explicit AudioEffectVSTHost(AudioEffectX& plugin);
    virtual void update(void);

    // One line for the effect and one per parameter, for a serial monitor.
    void printParameters(Print& out);

private:
    audio_block_t* inputQueue_[VSTEffect::MAX_CHANNELS];
};

// Holds the plug-in so it is constructed before the AudioStream base, which
// asks it how many inputs it has.
template <class Plugin>
struct AudioEffectVSTStorage {
    AudioEffectVSTStorage() : owned() {}
    VSTPlugin<Plugin> owned;
};

template <class Plugin>
class AudioEffectVST : private AudioEffectVSTStorage<Plugin>, public AudioEffectVSTHost {
public:
    AudioEffectVST() : AudioEffectVSTStorage<Plugin>(), AudioEffectVSTHost(this->owned) {}
    Plugin& plugin() { return this->owned; }
};

// A rack of plug-ins as one Audio Library object: the input blocks are
// converted to float once, run through every stage, and converted back
// once. Per-slot processor use is measured with the cycle counter, as the
// Audio Library measures its objects.
class AudioEffectVSTChain : public AudioStream, public VSTRack {
public:
    AudioEffectVSTChain();
    virtual void update(void);

    float processorUsage(int n) const;
    float processorUsageMax(int n) const;
    void processorUsageMaxReset() { resetCyclesMax(); }

    // Every slot, numbered, each as AudioEffectVSTHost::printParameters
    // with its stage, gain and mix.
    void printParameters(Print& out);

private:
    audio_block_t* inputQueue_[VSTEffect::MAX_CHANNELS];
};

#endif
