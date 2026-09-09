#ifndef __AND_H_
#define __AND_H_

#include "hardware.h"
#include "gpio.h"
#include "algorithm/algorithm.h"

class LogicNot : public Algorithm{
    public:
        LogicNot(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(midi_in, midi_out, gate_in, gate_out),
            input_pin_index(0)
            {
                for(uint8_t i=0; i<GPIO_N; i++){
                    if ((gate_in & (1 << i)) != 0){ input_pin_index = i; }
                }

            };

    private:
        uint8_t input_pin_index;
        void _update(){
            uint8_t state = !gpioDigitalRead(LogicNot::input_pin_index);
            gpioMapDigitalWrite(LogicNot::gate_outputs, state);
        };

};

class LogicGate : public Algorithm{
    public:
        LogicGate(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(midi_in, midi_out, gate_in, gate_out){};

    protected:
        uint8_t state = 0;
        bool inverted = false;

    private:
        void _update(){
            uint8_t input_state[GPIO_N] = {0};
            gpioMapDigitalRead(LogicGate::gate_inputs, &input_state[0]);

            uint8_t mask = 0;
            state = 1;
            for (uint8_t i=0; i<GPIO_N; i++){
                mask = (1 << i);
                if (mask > gate_inputs) break;

                if ((mask & gate_inputs) == mask) {
                    state = operate(state, input_state[i]);
                }
            }
            if (inverted) state = !state;
            gpioMapDigitalWrite(LogicGate::gate_outputs, state);
        };

        uint8_t operate(uint8_t acc, uint8_t input){
            acc &= input;
            return acc;
        }
};

class LogicAND : public LogicGate{
    public:
        LogicAND(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicGate(midi_in, midi_out, gate_in, gate_out){};

        uint8_t operate(uint8_t acc, uint8_t input){
            acc &= input;
            return acc;
        }
};

class LogicNAND : public LogicAND{
    public:
        LogicNAND(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicAND(midi_in, midi_out, gate_in, gate_out){ inverted = true; };
};


class LogicOR : public LogicGate{
    public:
        LogicOR(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicGate(midi_in, midi_out, gate_in, gate_out){};

        uint8_t operate(uint8_t acc, uint8_t input){
            acc |= input;
            return acc;
        }
};

class LogicNOR : public LogicOR{
    public:
        LogicNOR(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicOR(midi_in, midi_out, gate_in, gate_out){ inverted = true; };
};

class LogicXOR : public LogicGate{
    public:
        LogicXOR(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicGate(midi_in, midi_out, gate_in, gate_out){};

        uint8_t operate(uint8_t acc, uint8_t input){
            acc ^= input;
            return acc;
        }
};

class LogicXNOR : public LogicXOR{
    public:
        LogicXNOR(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicXOR(midi_in, midi_out, gate_in, gate_out){ inverted = true; };
};

#endif
