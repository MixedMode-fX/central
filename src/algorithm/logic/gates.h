#ifndef __GATES_H_
#define __GATES_H_

#include "algorithm/algorithm.h"

// Inverter: exactly one input port, any number of output ports.
class LogicNot : public Algorithm{
    public:
        LogicNot(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out) :
            Algorithm(gpio_if, gate_in, gate_out),
            input_port(single_port(gate_in))
            {
                if (input_port == NO_PORT) reject();
            };

    private:
        uint8_t input_port;
        void _update(uint32_t) override {
            const uint8_t state = gpio.read(input_port) ? GPIO_LOW : GPIO_HIGH;
            gpio_map_write(gpio, gate_outputs, state);
        };
};

// N-input gate: folds operate() over every input in the mask, starting from
// the gate's identity element. XOR over more than two inputs is therefore
// parity (odd number of high inputs -> high), see README.
class LogicGate : public Algorithm{
    public:
        LogicGate(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out, bool invert) :
            Algorithm(gpio_if, gate_in, gate_out),
            inverted(invert){};

    protected:
        virtual uint8_t operate(uint8_t acc, uint8_t input) const = 0;
        virtual uint8_t identity() const = 0;   // AND/NAND -> 1; OR/NOR/XOR/XNOR -> 0

    private:
        bool inverted;

        void _update(uint32_t) override {
            uint8_t input_state[GPIO_N] = {0};
            gpio_map_read(gpio, gate_inputs, &input_state[0]);

            uint8_t state = identity();
            for (uint8_t port = 0; port < GPIO_N; port++){
                const uint16_t mask = (uint16_t)(1u << port);
                if (mask > gate_inputs) break;
                if ((mask & gate_inputs) == mask) state = operate(state, input_state[port]);
            }
            if (inverted) state = state ? GPIO_LOW : GPIO_HIGH;
            gpio_map_write(gpio, gate_outputs, state);
        };
};

class LogicAND : public LogicGate{
    public:
        LogicAND(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out, bool invert = false) :
            LogicGate(gpio_if, gate_in, gate_out, invert){};
    protected:
        uint8_t operate(uint8_t acc, uint8_t input) const override { return acc & input; }
        uint8_t identity() const override { return 1; }
};

class LogicNAND : public LogicAND{
    public:
        LogicNAND(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out) :
            LogicAND(gpio_if, gate_in, gate_out, true){};
};

class LogicOR : public LogicGate{
    public:
        LogicOR(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out, bool invert = false) :
            LogicGate(gpio_if, gate_in, gate_out, invert){};
    protected:
        uint8_t operate(uint8_t acc, uint8_t input) const override { return acc | input; }
        uint8_t identity() const override { return 0; }
};

class LogicNOR : public LogicOR{
    public:
        LogicNOR(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out) :
            LogicOR(gpio_if, gate_in, gate_out, true){};
};

class LogicXOR : public LogicGate{
    public:
        LogicXOR(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out, bool invert = false) :
            LogicGate(gpio_if, gate_in, gate_out, invert){};
    protected:
        uint8_t operate(uint8_t acc, uint8_t input) const override { return acc ^ input; }
        uint8_t identity() const override { return 0; }
};

class LogicXNOR : public LogicXOR{
    public:
        LogicXNOR(IGpio& gpio_if, uint16_t gate_in, uint16_t gate_out) :
            LogicXOR(gpio_if, gate_in, gate_out, true){};
};

#endif
