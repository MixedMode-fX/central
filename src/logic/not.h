#ifndef __NOT_H_
#define __NOT_H_

#include "hardware.h"
#include "gpio.h"
#include "algorithm.h"

class LogicNot : public Algorithm{
    public:
        LogicNot(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            Algorithm(midi_inputs, midi_outputs, gate_inputs, gate_outputs){
                for(uint8_t i=0; i<GPIO_N; i++){
                    if ((gate_inputs & (1 << i)) != 0){ input_pin_index = i; }
                }

            };
        void update(){
            uint8_t state = !gpioDigitalRead(LogicNot::input_pin_index);
            gpioMapDigitalWrite(LogicNot::gate_outputs, state);
        };

    private:
        uint8_t input_pin_index;
};



#endif