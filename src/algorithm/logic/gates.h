#ifndef __GATES_H_
#define __GATES_H_

#include "node/node.h"

// Inverter: one gate inlet, one gate outlet.
class LogicNot : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicNot(const NodeConfig& config) :
            in(config.in_buses[0]), out(config.out_buses[0]) {}
        void process(BusManager& bus, uint32_t) override {
            bus.gate_write(out, !bus.gate_read(in));
        }
    private:
        BusSet in;
        BusSet out;
};

// N-input gate (up to MAX_IN inlets, unconnected ones are skipped): folds
// operate() over every connected inlet starting from the gate's identity
// element. XOR over more than two inputs is therefore parity, see README.
//
// An inlet reading several buses is their OR before the fold, because that is
// what a gate inlet reads (bus/domain.h) - so merging two triggers into one
// inlet of an AND is one input, not two.
class LogicGate : public Node{
    public:
        LogicGate(const NodeConfig& config, bool invert) :
            in(), out(config.out_buses[0]), inverted(invert) {
            for (uint8_t i = 0; i < MAX_IN; i++) in[i] = config.in_buses[i];
        }
        void process(BusManager& bus, uint32_t) override {
            bool state = identity();
            for (uint8_t i = 0; i < MAX_IN; i++){
                if (!in[i].any()) continue;
                state = operate(state, bus.gate_read(in[i]));
            }
            if (inverted) state = !state;
            bus.gate_write(out, state);
        }
    protected:
        virtual bool operate(bool acc, bool input) const = 0;
        virtual bool identity() const = 0;   // AND/NAND -> 1; OR/NOR/XOR/XNOR -> 0
    private:
        BusSet in[MAX_IN];
        BusSet out;
        bool inverted;
};

class LogicAND : public LogicGate{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicAND(const NodeConfig& config, bool invert = false) : LogicGate(config, invert) {}
    protected:
        bool operate(bool acc, bool input) const override { return acc && input; }
        bool identity() const override { return true; }
};

class LogicNAND : public LogicAND{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicNAND(const NodeConfig& config) : LogicAND(config, true) {}
};

class LogicOR : public LogicGate{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicOR(const NodeConfig& config, bool invert = false) : LogicGate(config, invert) {}
    protected:
        bool operate(bool acc, bool input) const override { return acc || input; }
        bool identity() const override { return false; }
};

class LogicNOR : public LogicOR{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicNOR(const NodeConfig& config) : LogicOR(config, true) {}
};

class LogicXOR : public LogicGate{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicXOR(const NodeConfig& config, bool invert = false) : LogicGate(config, invert) {}
    protected:
        bool operate(bool acc, bool input) const override { return acc != input; }
        bool identity() const override { return false; }
};

class LogicXNOR : public LogicXOR{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit LogicXNOR(const NodeConfig& config) : LogicXOR(config, true) {}
};

#endif
