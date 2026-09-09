#ifndef __SUSTAIN_H_
#define __SUSTAIN_H_

#include "hardware.h"
#include "gpio.h"
#include "mm_midi.h"
#include "algorithm/algorithm.h"

class Sustain : public Algorithm{
    public:
        Sustain(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(midi_in, midi_out, gate_in, gate_out),
            input_pin_index(0)
            {
                for(uint8_t i=0; i<GPIO_N; i++){
                    if ((gate_in & (1 << i)) != 0){ input_pin_index = i; }
                }
            };
        void set_invert(bool inv){ invert = inv; }

    private:
        void _update();
        uint8_t input_pin_index;
        uint8_t state = 1;
        bool invert = false;
};

#endif
