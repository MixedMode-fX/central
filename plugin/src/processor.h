#ifndef MMMC_PLUGIN_PROCESSOR_H
#define MMMC_PLUGIN_PROCESSOR_H

// The engine first: a system header JUCE pulls in defines MOD_OFFSET as a
// macro, and the firmware's ModMode (node/patch.h) names an enumerator so.
// Declared before the macro exists, the enum is untouched, and nothing here
// spells the name again.
#include "engine.h"

#include <memory>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>

// The DAW's end of the module: one of the two files that knows what a host
// is. It turns a block into what the engine takes - samples, messages at
// sample offsets, the playhead as a clock - and what the engine made into
// the host's MIDI output. The other file is the editor.
//
// **Threads.** The host runs processBlock on its audio thread; the editor and
// the host's state calls arrive on the message thread. The firmware is one
// loop with the control plane between passes, so the engine is one object
// under one lock: a pass, a SysEx message and a state save each take it
// whole and briefly. The critical sections on the message thread are
// bounded - a dump encodes a kilobyte - which is what makes a plain lock
// on the audio thread the honest choice over a queue that would leave the
// editor waiting for a block the host has stopped sending.
class MmmcProcessor : public juce::AudioProcessor {
    public:
        MmmcProcessor();
        ~MmmcProcessor() override;

        void prepareToPlay(double sampleRate, int samplesPerBlock) override;
        void releaseResources() override {}
        bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
        void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

        juce::AudioProcessorEditor* createEditor() override;
        bool hasEditor() const override { return true; }

        const juce::String getName() const override { return JucePlugin_Name; }
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return true; }
        bool isMidiEffect() const override { return false; }
        double getTailLengthSeconds() const override { return 0.0; }

        // The patch is the program, and it has a protocol of its own.
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}

        // The module's memory, whole (engine.h).
        void getStateInformation(juce::MemoryBlock& destData) override;
        void setStateInformation(const void* data, int sizeInBytes) override;

        // --- the editor's side ----------------------------------------------

        // One SysEx message from the window, answered now: `replies` gets
        // everything the module said in return, and anything it had said on
        // its own since the last drain, as F0..F7 runs.
        void editorSysex(const uint8_t* data, size_t length, std::vector<uint8_t>& replies);
        // A channel message from the window, on the cable it names, for the
        // next pass.
        void editorMidi(uint8_t source, uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2);
        // What the module said on its own - a recall, a learn, a patch the
        // host restored - since the last call.
        void drainEditor(std::vector<uint8_t>& replies);

    private:
        // Messages a block can carry into the engine: the host's MIDI and
        // the clock, merged. Reserved once; a block past it drops the tail.
        static constexpr size_t EVENTS = 4096;
        static constexpr uint32_t CLOCKS = 256;

        void takeEditorSysex(std::vector<uint8_t>& replies);

        std::unique_ptr<mmmc_plugin::Engine> engine;
        juce::CriticalSection lock;
        mmmc_plugin::HostClock hostClock;
        std::vector<mmmc_plugin::InEvent> events;
        std::vector<mmmc_plugin::InEvent> clocks;
        std::vector<uint8_t> state;
        double sampleRate;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MmmcProcessor)
};

#endif
