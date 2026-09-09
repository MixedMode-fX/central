#ifndef __SUSTAIN_H_
#define __SUSTAIN_H_

#include "algorithm/algorithm.h"
#include "hal/imidi_out.h"

class Sustain : public Algorithm{
    public:
        Sustain(IGpio& gpio_if, IMidiOut& midi_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(gpio_if, midi_in, midi_out, gate_in, gate_out),
            midi(midi_if),
            input_pin_index(0)
            {
                for(uint8_t i=0; i<GPIO_N; i++){
                    if ((gate_in & (1 << i)) != 0){ input_pin_index = i; }
                }
            };
        void set_invert(bool inv){ invert = inv; }

    private:
        void _update();
        IMidiOut& midi;
        uint8_t input_pin_index;
        uint8_t state = 1;
        bool invert = false;
};

#endif
