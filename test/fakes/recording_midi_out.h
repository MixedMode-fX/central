#ifndef MMMC_TEST_RECORDING_MIDI_OUT_H
#define MMMC_TEST_RECORDING_MIDI_OUT_H

#include <vector>
#include "hal/imidi_out.h"

// IMidiOut for the native tests: records every message with its target mask.
class RecordingMidiOut : public IMidiOut {
    public:
        struct Message {
            uint8_t target;
            uint8_t type;
            uint8_t d1;
            uint8_t d2;
            uint8_t channel;
        };

        static constexpr size_t RESERVE = 65536;

        RecordingMidiOut() : messages() {
            messages.reserve(RESERVE);   // so recording never allocates mid-test
        }

        void send(uint8_t target, uint8_t type,
                  uint8_t d1, uint8_t d2, uint8_t channel) override {
            messages.push_back(Message{target, type, d1, d2, channel});
        }

        void clear() { messages.clear(); }

        std::vector<Message> messages;
};

#endif
