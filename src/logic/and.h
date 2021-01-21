#ifndef __AND_H_
#define __AND_H_

#include "hardware.h"
#include "gpio.h"
#include "algorithm.h"

class LogicAND : public Algorithm{
    public:
        LogicAND(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            Algorithm(midi_inputs, midi_outputs, gate_inputs, gate_outputs){};
        void update(){
            compute();
            gpioMapDigitalWrite(LogicAND::gate_outputs, state);
        };
        void compute(){
            uint8_t input_state[GPIO_N] = {0};
            gpioMapDigitalRead(LogicAND::gate_inputs, &input_state[0]);

            uint8_t mask = 0;
            state = 1;
            for (uint8_t i=0; i<GPIO_N; i++){
                mask = (1 << i);
                if (mask > gate_inputs) break;

                if ((mask & gate_inputs) == mask) {
                    state &= input_state[i];                  
                }
            }
        }

    protected:
        uint8_t state = 0;
};

class LogicNAND : public LogicAND{
    public:
        LogicNAND(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            LogicAND(midi_inputs, midi_outputs, gate_inputs, gate_outputs){};
        void update() {
            compute();
            gpioMapDigitalWrite(LogicAND::gate_outputs, !state);
        };
};


#endif