#ifndef __AND_H_
#define __AND_H_

#include "algorithm/algorithm.h"

class LogicNot : public Algorithm{
    public:
        LogicNot(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(gpio_if, midi_in, midi_out, gate_in, gate_out),
            input_pin_index(0)
            {
                for(uint8_t i=0; i<GPIO_N; i++){
                    if ((gate_in & (1 << i)) != 0){ input_pin_index = i; }
                }

            };

    private:
        uint8_t input_pin_index;
        void _update(){
            uint8_t state = !gpio.read(LogicNot::input_pin_index);
            gpio_map_write(gpio, LogicNot::gate_outputs, state);
        };

};

class LogicGate : public Algorithm{
    public:
        LogicGate(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(gpio_if, midi_in, midi_out, gate_in, gate_out){};

    protected:
        uint8_t state = 0;
        bool inverted = false;

    private:
        void _update(){
            uint8_t input_state[GPIO_N] = {0};
            gpio_map_read(gpio, LogicGate::gate_inputs, &input_state[0]);

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
            gpio_map_write(gpio, LogicGate::gate_outputs, state);
        };

        uint8_t operate(uint8_t acc, uint8_t input){
            acc &= input;
            return acc;
        }
};

class LogicAND : public LogicGate{
    public:
        LogicAND(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicGate(gpio_if, midi_in, midi_out, gate_in, gate_out){};

        uint8_t operate(uint8_t acc, uint8_t input){
            acc &= input;
            return acc;
        }
};

class LogicNAND : public LogicAND{
    public:
        LogicNAND(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicAND(gpio_if, midi_in, midi_out, gate_in, gate_out){ inverted = true; };
};


class LogicOR : public LogicGate{
    public:
        LogicOR(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicGate(gpio_if, midi_in, midi_out, gate_in, gate_out){};

        uint8_t operate(uint8_t acc, uint8_t input){
            acc |= input;
            return acc;
        }
};

class LogicNOR : public LogicOR{
    public:
        LogicNOR(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicOR(gpio_if, midi_in, midi_out, gate_in, gate_out){ inverted = true; };
};

class LogicXOR : public LogicGate{
    public:
        LogicXOR(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicGate(gpio_if, midi_in, midi_out, gate_in, gate_out){};

        uint8_t operate(uint8_t acc, uint8_t input){
            acc ^= input;
            return acc;
        }
};

class LogicXNOR : public LogicXOR{
    public:
        LogicXNOR(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            LogicXOR(gpio_if, midi_in, midi_out, gate_in, gate_out){ inverted = true; };
};

#endif
