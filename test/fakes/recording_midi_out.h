#ifndef MMMC_TEST_RECORDING_MIDI_OUT_H
#define MMMC_TEST_RECORDING_MIDI_OUT_H

#include <vector>
#include "hal/imidi_out.h"
#include "protocol/sysex.h"

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

        RecordingMidiOut() : messages(), sysex() {
            messages.reserve(RESERVE);   // so recording never allocates mid-test
            sysex.reserve(1024);
        }

        // One SysEx reply, recorded whole so a test can parse it the way an
        // editor would.
        struct Sysex {
            uint8_t target;
            std::vector<uint8_t> bytes;
        };

        void send(uint8_t target, uint8_t type,
                  uint8_t d1, uint8_t d2, uint8_t channel) override {
            messages.push_back(Message{target, type, d1, d2, channel});
        }

        void send_sysex(uint8_t target, const uint8_t* data, uint16_t length) override {
            sysex.push_back(Sysex{target, std::vector<uint8_t>(data, data + length)});
        }

        void clear() { messages.clear(); sysex.clear(); }

        // The last reply carrying `command`, or nullptr.
        const Sysex* last_reply(uint8_t command) const {
            for (size_t i = sysex.size(); i > 0; i--) {
                const Sysex& s = sysex[i - 1];
                if (s.bytes.size() > 3 && s.bytes[1] == SYSEX_MANUFACTURER && s.bytes[3] == command) return &s;
            }
            return nullptr;
        }
        size_t count_replies(uint8_t command) const {
            size_t n = 0;
            for (const Sysex& s : sysex) {
                if (s.bytes.size() > 3 && s.bytes[1] == SYSEX_MANUFACTURER && s.bytes[3] == command) n++;
            }
            return n;
        }

        std::vector<Message> messages;
        std::vector<Sysex> sysex;
};

#endif
