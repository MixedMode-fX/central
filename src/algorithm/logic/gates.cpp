#include "algorithm/logic/gates.h"
#include "node/registry.h"

static const Domain GATE_INLETS[MAX_IN] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };
static_assert(MAX_IN == 5, "GATE_INLETS lists one domain per inlet");
static const Domain GATE_OUTLET[1] = {Domain::Gate};

// The first inlet is the one that must be connected (min_in is 1); the rest
// are folded in if they are patched, which is what "unconnected inputs are
// skipped" means where a user can see it.
static const char* const GATE_IN_NAMES[MAX_IN] = {"in 1", "in 2", "in 3", "in 4", "in 5"};
static const char* const NOT_IN_NAMES[1] = {"in"};
static const char* const GATE_OUT_NAMES[1] = {"out"};

#define GATE_DESCRIPTOR(Class, Id, Name, NIn, InNames, Summary) \
    const AlgorithmDescriptor Class::descriptor = { \
        Id, Name, NIn, 1, 1, 0, GATE_INLETS, GATE_OUTLET, sizeof(Class), false, construct_node<Class>, \
        nullptr, 0, InNames, GATE_OUT_NAMES, Summary, CATEGORY_LOGIC };

GATE_DESCRIPTOR(LogicNot,  ALGO_LOGIC_NOT,  "NOT",  1, NOT_IN_NAMES,
                "Inverts a gate: the outlet is high whenever the inlet is low.")
GATE_DESCRIPTOR(LogicAND,  ALGO_LOGIC_AND,  "AND",  MAX_IN, GATE_IN_NAMES,
                "High while every connected inlet is high. Unconnected inlets are skipped.")
GATE_DESCRIPTOR(LogicNAND, ALGO_LOGIC_NAND, "NAND", MAX_IN, GATE_IN_NAMES,
                "AND inverted: low only while every connected inlet is high.")
GATE_DESCRIPTOR(LogicOR,   ALGO_LOGIC_OR,   "OR",   MAX_IN, GATE_IN_NAMES,
                "High while any connected inlet is high. Merges several triggers onto one bus.")
GATE_DESCRIPTOR(LogicNOR,  ALGO_LOGIC_NOR,  "NOR",  MAX_IN, GATE_IN_NAMES,
                "OR inverted: high only while every connected inlet is low.")
GATE_DESCRIPTOR(LogicXOR,  ALGO_LOGIC_XOR,  "XOR",  MAX_IN, GATE_IN_NAMES,
                "Parity: high when an odd number of connected inlets is high.")
GATE_DESCRIPTOR(LogicXNOR, ALGO_LOGIC_XNOR, "XNOR", MAX_IN, GATE_IN_NAMES,
                "XOR inverted: high when an even number of connected inlets is high.")
