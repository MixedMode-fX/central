#ifndef __ALGORITHM_H_
#define __ALGORITHM_H_

#include <stdint.h>
#include "config.h"
#include "hal/igpio.h"
#include "hal/gpio_map.h"

// Interim base class: algorithms still address hardware ports by mask and
// hold the IGpio they were given. #9 replaces the masks with bus indices and
// moves the IGpio into the hardware port nodes.
class Algorithm{
    public:
        Algorithm(IGpio& gpio_if, uint8_t midi_in, uint8_t midi_out, uint16_t gate_in, uint16_t gate_out) :
            gpio(gpio_if),
            midi_inputs(midi_in),
            midi_outputs(midi_out),
            gate_inputs(gate_in),
            gate_outputs(gate_out),
            bypass(false)
        {
            gpio_map_mode(gpio, gate_inputs, GPIO_MODE_INPUT_PULLUP);
            gpio_map_mode(gpio, gate_outputs, GPIO_MODE_OUTPUT);
        };
        virtual ~Algorithm(){
            gpio_map_mode(gpio, Algorithm::gate_inputs + Algorithm::gate_outputs, GPIO_MODE_OUTPUT);
            gpio_map_write(gpio, Algorithm::gate_inputs + Algorithm::gate_outputs, GPIO_LOW);
        };
        Algorithm(const Algorithm&) = delete;
        Algorithm& operator=(const Algorithm&) = delete;

        void update(){ if(!Algorithm::bypass) _update(); };

        void set_bypass(bool b){ Algorithm::bypass = b; }
        void toggle_bypass(){ set_bypass(!Algorithm::bypass); }

    protected:
        IGpio& gpio;
        uint8_t midi_inputs;
        uint8_t midi_outputs;
        uint16_t gate_inputs;
        uint16_t gate_outputs;

    private:
        virtual void _update(){};
        bool bypass;
};

#endif
