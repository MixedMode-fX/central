#ifndef __ALGORITHM_H_
#define __ALGORITHM_H_

#include <Arduino.h>

class Algorithm{
    public:
        Algorithm(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs){
            Algorithm::midi_inputs = midi_inputs;
            Algorithm::midi_outputs = midi_outputs;
            Algorithm::gate_inputs = gate_inputs;
            Algorithm::gate_outputs = gate_outputs;

            gpioMapMode(gate_inputs, INPUT_PULLUP);
            gpioMapMode(gate_outputs, OUTPUT);
        };
        ~Algorithm(){
            gpioMapMode(Algorithm::gate_inputs + Algorithm::gate_outputs, OUTPUT);
            gpioMapDigitalWrite(Algorithm::gate_inputs + Algorithm::gate_outputs, LOW);
        };
        virtual void update(){};

    protected:
        uint8_t midi_inputs;
        uint8_t midi_outputs;
        uint16_t gate_inputs;
        uint16_t gate_outputs;
};



#endif