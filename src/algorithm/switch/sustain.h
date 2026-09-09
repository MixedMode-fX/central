#ifndef __SUSTAIN_H_
#define __SUSTAIN_H_

#include "algorithm/algorithm.h"
#include "hal/imidi_out.h"

// Sustain pedal to MIDI CC. Exactly one input port; sends a control change
// (default CC64, channel 1) to every port in `midi_targets` whenever the
// debounced pedal state changes, and once at setup() so a host that starts
// with the pedal already down learns about it.
class Sustain : public Algorithm{
    public:
        static constexpr uint32_t DEFAULT_DEBOUNCE_US = 5000;

        Sustain(IGpio& gpio_if, IMidiOut& midi_if, uint16_t gate_in, uint8_t midi_targets) :
            Algorithm(gpio_if, gate_in, 0),
            midi(midi_if),
            targets(midi_targets),
            input_port(single_port(gate_in)),
            channel(1),
            controller(64),
            invert(false),
            debounce_us(DEFAULT_DEBOUNCE_US),
            state(0),
            last_raw(0),
            last_change_us(0)
            {
                if (input_port == NO_PORT) reject();
            };

        void set_channel(uint8_t ch){ channel = ch; }
        void set_controller(uint8_t cc){ controller = cc; }
        // For normally-closed pedals: the reported state is the inverse of the input.
        void set_invert(bool inv){ invert = inv; }
        void set_debounce_us(uint32_t us){ debounce_us = us; }

        uint8_t pedal_down() const { return state; }

    private:
        void _setup() override;
        void _update(uint32_t now_us) override;
        void send_state();

        IMidiOut& midi;
        uint8_t targets;
        uint8_t input_port;
        uint8_t channel;
        uint8_t controller;
        bool invert;
        uint32_t debounce_us;
        uint8_t state;          // debounced, after inversion: 1 = pedal down
        uint8_t last_raw;       // last raw read, after inversion
        uint32_t last_change_us;
};

#endif
