#ifndef MMMC_PLUGIN_EDITOR_H
#define MMMC_PLUGIN_EDITOR_H

// The processor first, which is the engine first: see processor.h for the
// macro JUCE's system headers would otherwise put in the firmware's way.
#include "processor.h"

#include <vector>
#include <juce_gui_extra/juce_gui_extra.h>

// The editor is the app (app/README.md), in a web view, with the plugin as
// the module it is talking to. Nothing is drawn natively: the page is the
// one-file build emulator/build.sh produces, embedded at build time, and it
// finds out it is inside a plugin the way it finds out about hardware - by
// what answers when it asks. The bridge carries the protocol's own bytes in
// both directions and a channel message from the page's keyboard, which is
// what a cable carries, so the app is the same client it is everywhere
// (app/src/runtime/host.js is its end of this).
class MmmcEditor : public juce::AudioProcessorEditor, private juce::Timer {
    public:
        explicit MmmcEditor(MmmcProcessor& processor);
        ~MmmcEditor() override;

        void resized() override;

    private:
        // How often the page is handed what the module said on its own.
        static constexpr int POLL_MS = 30;

        juce::WebBrowserComponent::Options options();
        void onSysex(const juce::var& payload);
        void onMidi(const juce::var& payload);
        void emit(const std::vector<uint8_t>& runs);
        void timerCallback() override;

        MmmcProcessor& processor;
        juce::WebBrowserComponent browser;
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> replies;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MmmcEditor)
};

#endif
