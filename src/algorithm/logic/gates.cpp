#include "algorithm/logic/gates.h"
#include "node/registry.h"

static const Domain GATE_INLETS[MAX_IN] = {Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain GATE_OUTLET[1] = {Domain::Gate};

#define GATE_DESCRIPTOR(Class, Id, Name, NIn) \
    const AlgorithmDescriptor Class::descriptor = { \
        Id, Name, NIn, 1, 1, 0, GATE_INLETS, GATE_OUTLET, sizeof(Class), false, construct_node<Class>, \
        nullptr, 0 };

GATE_DESCRIPTOR(LogicNot,  ALGO_LOGIC_NOT,  "NOT",  1)
GATE_DESCRIPTOR(LogicAND,  ALGO_LOGIC_AND,  "AND",  MAX_IN)
GATE_DESCRIPTOR(LogicNAND, ALGO_LOGIC_NAND, "NAND", MAX_IN)
GATE_DESCRIPTOR(LogicOR,   ALGO_LOGIC_OR,   "OR",   MAX_IN)
GATE_DESCRIPTOR(LogicNOR,  ALGO_LOGIC_NOR,  "NOR",  MAX_IN)
GATE_DESCRIPTOR(LogicXOR,  ALGO_LOGIC_XOR,  "XOR",  MAX_IN)
GATE_DESCRIPTOR(LogicXNOR, ALGO_LOGIC_XNOR, "XNOR", MAX_IN)
