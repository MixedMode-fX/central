#ifndef __SUSTAIN_H_
#define __SUSTAIN_H_

#include "hardware.h"
#include "gpio.h"
#include "mm_midi.h"
#include "algorithm/algorithm.h"

class Sustain : public Algorithm{
    public:
        Sustain(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            Algorithm(midi_inputs, midi_outputs, gate_inputs, gate_outputs){
                for(uint8_t i=0; i<GPIO_N; i++){
                    if ((gate_inputs & (1 << i)) != 0){ input_pin_index = i; }
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