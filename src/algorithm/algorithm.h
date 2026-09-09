#ifndef __ALGORITHM_H_
#define __ALGORITHM_H_

#include "hal/teensy/teensy_includes.h"
#include "gpio.h"

class Algorithm{
    public:
        Algorithm(uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            midi_inputs(midi_in),
            midi_outputs(midi_out),
            gate_inputs(gate_in),
            gate_outputs(gate_out),
            bypass(false)
        {
            gpioMapMode(gate_inputs, INPUT_PULLUP);
            gpioMapMode(gate_outputs, OUTPUT);
        };
        virtual ~Algorithm(){
            gpioMapMode(Algorithm::gate_inputs + Algorithm::gate_outputs, OUTPUT);
            gpioMapDigitalWrite(Algorithm::gate_inputs + Algorithm::gate_outputs, LOW);
        };
        void update(){ if(!Algorithm::bypass) _update(); };

        void set_bypass(bool b){ Algorithm::bypass = b; }
        void toggle_bypass(){ set_bypass(!Algorithm::bypass); }

    protected:
        uint8_t midi_inputs;
        uint8_t midi_outputs;
        uint16_t gate_inputs;
        uint16_t gate_outputs;

    private:
        virtual void _update(){};
        bool bypass;
};



#endif
