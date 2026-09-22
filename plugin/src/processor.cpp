#include "processor.h"
#include "editor.h"
#include "hal/midi_types.h"

using mmmc_plugin::Engine;
using mmmc_plugin::InEvent;
using mmmc_plugin::HOST_IN;

MmmcProcessor::MmmcProcessor() :
    // An instrument to the host: a stereo output, of silence, so the plugin
    // sits on an instrument track where MIDI comes in and goes out. What it
    // makes is the MIDI.
    AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
    engine(std::make_unique<Engine>()), lock(), hostClock(), events(), clocks(),
    state(), sampleRate(48000.0)
{
    events.reserve(EVENTS);
    clocks.resize(CLOCKS);
    state.resize(Engine::state_capacity());
    // main.cpp stirs the pool from a cycle counter and a floating ADC; a
    // plugin instance has the clock and the system's own randomness.
    engine->boot((uint32_t)juce::Random::getSystemRandom().nextInt(),
                 (uint32_t)juce::Time::currentTimeMillis());
}

MmmcProcessor::~MmmcProcessor() {}

void MmmcProcessor::prepareToPlay(double newSampleRate, int){
    const juce::ScopedLock sl(lock);
    sampleRate = newSampleRate;
    engine->set_sample_rate(newSampleRate);
    hostClock.reset();
}

bool MmmcProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::disabled();
}

void MmmcProcessor::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi){
    const juce::ScopedLock sl(lock);
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    const int n = audio.getNumSamples();
    if (n <= 0){ midi.clear(); return; }

    // 1. the playhead, as the realtime a slaved module hears.
    bool playing = false;
    double ppq = 0.0, bpm = 0.0;
    if (auto* head = getPlayHead()){
        if (const auto position = head->getPosition()){
            playing = position->getIsPlaying();
            ppq = position->getPpqPosition().orFallback(0.0);
            bpm = position->getBpm().orFallback(0.0);
        }
    }
    const uint32_t nClocks = hostClock.block(playing, ppq, bpm, sampleRate, (uint32_t)n,
                                             clocks.data(), CLOCKS);

    // 2. the host's MIDI, merged with the clock in sample order. Its own
    //    realtime bytes are dropped: the playhead is the transport here,
    //    and two masters on one cable is what the module's clock mask
    //    exists to prevent. SysEx waits for step 4.
    events.clear();
    uint32_t c = 0;
    for (const auto metadata : midi){
        const auto message = metadata.getMessage();
        const uint8_t* raw = message.getRawData();
        const int size = message.getRawDataSize();
        if (size < 1) continue;
        const uint8_t status = raw[0];
        if (status >= 0xF0) continue;
        const uint32_t sample = (uint32_t)juce::jmax(0, metadata.samplePosition);
        while (c < nClocks && clocks[c].sample <= sample && events.size() < EVENTS) events.push_back(clocks[c++]);
        if (events.size() >= EVENTS) break;
        InEvent e;
        e.sample = sample;
        e.source = HOST_IN;
        e.type = (uint8_t)(status & 0xF0);
        e.channel = (uint8_t)((status & 0x0F) + 1);
        e.d1 = size > 1 ? raw[1] : 0;
        e.d2 = size > 2 ? raw[2] : 0;
        events.push_back(e);
    }
    while (c < nClocks && events.size() < EVENTS) events.push_back(clocks[c++]);

    // 3. the passes the block covers.
    engine->process((uint32_t)n, events.data(), (uint32_t)events.size());

    // 4. SysEx on the host's wire is the protocol, handled after the passes
    //    - which clear the block's replies as they start - and answered on
    //    the same wire below.
    for (const auto metadata : midi){
        const auto message = metadata.getMessage();
        if (message.isSysEx()) engine->receive_sysex(HOST_IN, message.getRawData(), (uint32_t)message.getRawDataSize());
    }

    // 5. what the module sent, at the sample of the pass that sent it.
    midi.clear();
    const auto& out = engine->midi();
    for (uint32_t i = 0; i < out.n_events; i++){
        const auto& e = out.events[i];
        const int at = (int)juce::jmin<uint32_t>(e.sample, (uint32_t)(n - 1));
        switch (e.length){
            case 1:  midi.addEvent(juce::MidiMessage(e.status), at); break;
            case 2:  midi.addEvent(juce::MidiMessage(e.status, e.d1), at); break;
            default: midi.addEvent(juce::MidiMessage(e.status, e.d1, e.d2), at); break;
        }
    }
    // A reply on the host's wire, one message per F0..F7 run.
    size_t start = 0;
    for (size_t i = 0; i < out.host_sysex_used; i++){
        if (out.host_sysex[i] == 0xF7){
            midi.addEvent(juce::MidiMessage(out.host_sysex + start, (int)(i + 1 - start)), 0);
            start = i + 1;
        }
    }
}

// --- state ---------------------------------------------------------------------

void MmmcProcessor::getStateInformation(juce::MemoryBlock& destData){
    const juce::ScopedLock sl(lock);
    const size_t written = engine->write_state(state.data(), state.size());
    destData.setSize(0);
    if (written) destData.append(state.data(), written);
}

void MmmcProcessor::setStateInformation(const void* data, int sizeInBytes){
    if (data == nullptr || sizeInBytes <= 0) return;
    const juce::ScopedLock sl(lock);
    engine->read_state(static_cast<const uint8_t*>(data), (size_t)sizeInBytes);
}

// --- the editor ------------------------------------------------------------

juce::AudioProcessorEditor* MmmcProcessor::createEditor(){ return new MmmcEditor(*this); }

void MmmcProcessor::takeEditorSysex(std::vector<uint8_t>& replies){
    auto& out = engine->midi();
    replies.assign(out.editor_sysex, out.editor_sysex + out.editor_sysex_used);
    out.clear_editor_sysex();
}

void MmmcProcessor::editorSysex(const uint8_t* data, size_t length, std::vector<uint8_t>& replies){
    const juce::ScopedLock sl(lock);
    engine->receive_sysex(MIDI_CONTROL_PORT, data, (uint32_t)length);
    takeEditorSysex(replies);
}

void MmmcProcessor::editorMidi(uint8_t source, uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2){
    const juce::ScopedLock sl(lock);
    engine->receive(source, type, channel, d1, d2);
}

void MmmcProcessor::drainEditor(std::vector<uint8_t>& replies){
    const juce::ScopedLock sl(lock);
    if (engine->midi().editor_sysex_used == 0){ replies.clear(); return; }
    takeEditorSysex(replies);
}

// The host asks for one of these; JUCE's wrappers call it.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){ return new MmmcProcessor(); }
