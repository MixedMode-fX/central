#ifndef __XOR_H_
#define __XOR_H_

#include "hardware.h"
#include "gpio.h"
#include "algorithm.h"

class LogicXOR : public Algorithm{
    public:
        LogicXOR(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            Algorithm(midi_inputs, midi_outputs, gate_inputs, gate_outputs){};
        void update(){
            compute();
            gpioMapDigitalWrite(LogicXOR::gate_outputs, state);
        };

        void compute(){
            uint8_t input_state[GPIO_N] = {0};
            gpioMapDigitalRead(LogicXOR::gate_inputs, &input_state[0]);

            uint8_t mask = 0;
            state = 0;
            for (uint8_t i=0; i<GPIO_N; i++){
                mask = (1 << i);
                if (mask > gate_inputs) break;

                if ((mask & gate_inputs) == mask) {
                    state ^= input_state[i];
                }
            }
        }

    protected:
        uint8_t state = 0;
};

class LogicNXOR : public LogicXOR{
    public:
        LogicNXOR(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            LogicXOR(midi_inputs, midi_outputs, gate_inputs, gate_outputs){};
        void update() {
            compute();
            gpioMapDigitalWrite(LogicXOR::gate_outputs, !state);
        };
};


#endif