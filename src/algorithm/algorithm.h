#ifndef __ALGORITHM_H_
#define __ALGORITHM_H_

#include <stdint.h>
#include "config.h"
#include "hal/igpio.h"
#include "hal/gpio_map.h"

// Sentinel for "no port": returned by single_port() when a mask does not
// select exactly one port.
#define NO_PORT 0xFF

// Interim base class: algorithms still address hardware ports by mask and
// hold the IGpio they were given. #9 replaces the masks with bus indices and
// moves the IGpio into the hardware port nodes.
//
// Lifecycle: construct (no hardware access), setup() claims the ports and
// runs _setup(), update(now_us) runs _update(now_us) once per pass, the
// destructor returns every claimed port to a safe (input) state.
class Algorithm{
    public:
        Algorithm(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out) :
            gpio(gpio_if),
            gate_inputs(gate_in),
            gate_outputs(gate_out),
            valid(true),
            bypass(false)
        {};
        virtual ~Algorithm(){
            gpio_map_mode(gpio, gate_inputs | gate_outputs, GPIO_MODE_INPUT_PULLUP);
        };
        Algorithm(const Algorithm&) = delete;
        Algorithm& operator=(const Algorithm&) = delete;

        // Claims the ports: inputs as pull-up inputs, outputs as outputs
        // driven low. Nothing touches the hardware before this is called.
        void setup(){
            if (!valid) return;
            gpio_map_mode(gpio, gate_inputs, GPIO_MODE_INPUT_PULLUP);
            gpio_map_mode(gpio, gate_outputs, GPIO_MODE_OUTPUT);
            gpio_map_write(gpio, gate_outputs, GPIO_LOW);
            _setup();
        }

        // `now_us` is a monotonic microsecond timestamp; it may wrap.
        void update(uint32_t now_us){ if(valid && !bypass) _update(now_us); };

        // False when the configuration was rejected at construction (for
        // example a single-input algorithm given a zero or multi-bit mask).
        // An invalid algorithm never touches the hardware.
        bool is_valid() const { return valid; }

        void set_bypass(bool b){ bypass = b; }
        void toggle_bypass(){ set_bypass(!bypass); }

        // The single port selected by `mask`, or NO_PORT if the mask selects
        // zero or several ports.
        static uint8_t single_port(uint16_t mask){
            uint8_t found = NO_PORT;
            for (uint8_t port = 0; port < GPIO_N; port++){
                if ((mask & (1u << port)) == 0) continue;
                if (found != NO_PORT) return NO_PORT;
                found = port;
            }
            return found;
        }

    protected:
        IGpio& gpio;
        uint16_t gate_inputs;
        uint16_t gate_outputs;

        // Derived classes call this from their constructor when they cannot
        // honour the configuration they were given.
        void reject(){ valid = false; }

    private:
        virtual void _setup(){};
        virtual void _update(uint32_t){};
        bool valid;
        bool bypass;
};

#endif
