#ifndef __SUSTAIN_H_
#define __SUSTAIN_H_

#include "node/node.h"

// Sustain pedal to MIDI CC. Reads one gate bus (the pedal), writes a control
// change to a note bus whenever the debounced level changes. The first
// stable level after load is transmitted too, so a host that starts with
// the pedal already down learns about it.
//
// params[0] channel (1..16, 0 -> 1)   params[1] controller (0 -> 64)
// params[2] invert (non-zero for a normally-closed pedal)
class Sustain : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint32_t DEBOUNCE_US = 5000;
        static constexpr uint8_t UNKNOWN = 0xFF;

        explicit Sustain(const NodeConfig& config);
        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t pedal_down() const { return state == UNKNOWN ? 0 : state; }

    private:
        void send(BusManager& bus) const;

        uint8_t in;
        uint8_t out;
        uint8_t channel;
        uint8_t controller;
        bool invert;
        uint8_t state;          // debounced, after inversion; UNKNOWN until settled
        uint8_t last_raw;
        uint32_t last_change_us;
};

#endif
