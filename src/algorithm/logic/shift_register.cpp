#include "algorithm/logic/shift_register.h"
#include "node/registry.h"

static const Domain IN[3] = {Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain OUT[ShiftRegister::TAPS] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate,
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };
static_assert(ShiftRegister::TAPS == 8, "OUT lists one domain per tap");

static const ParamDescriptor PARAMS[2] = {
    {"length", 1, ShiftRegister::TAPS, ShiftRegister::TAPS, PARAM_NUMBER, nullptr},
    {"loop",   0, 1, 0, PARAM_BOOL, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 2, PARAMS}};

static const char* const IN_NAMES[3] = {"clock", "data", "clear"};
static const char* const OUT_NAMES[ShiftRegister::TAPS] = {
    "tap 1", "tap 2", "tap 3", "tap 4", "tap 5", "tap 6", "tap 7", "tap 8"};

// The clock is required and data is not: a register with nothing on `data`
// and `loop` on is a loop that only a clear touches.
const AlgorithmDescriptor ShiftRegister::descriptor = {
    ALGO_SHIFT_REGISTER, "ShiftRegister", 3, 1, ShiftRegister::TAPS, 2, IN, OUT,
    sizeof(ShiftRegister), false, construct_node<ShiftRegister>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Eight bits shifted on every clock: a gate delayed k steps on tap k, or a loop played in.",
    CATEGORY_LOGIC };

ShiftRegister::ShiftRegister(const NodeConfig& config) :
    clock_in(config.in_buses[IN_CLOCK]),
    data_in(config.in_buses[IN_DATA]),
    clear_in(config.in_buses[IN_CLEAR]),
    out(),
    length(config.params[P_LENGTH] && config.params[P_LENGTH] <= TAPS ? config.params[P_LENGTH] : TAPS),
    loop(config.params[P_LOOP] != 0),
    reg(0)
{
    for (uint8_t t = 0; t < TAPS; t++) out[t] = config.out_buses[t];
}

void ShiftRegister::process(BusManager& bus, uint32_t){
    if (clock_in.rising(bus)){
        const bool fed_back = loop && ((reg >> (length - 1u)) & 1u);
        const bool in = (data_in.any() && bus.gate_read(data_in)) || fed_back;
        reg = (uint8_t)((reg << 1) | (in ? 1u : 0u));
    }
    // Clear wins over the shift in the same pass, as reset wins everywhere.
    if (clear_in.rising(bus)) reg = 0;
    for (uint8_t t = 0; t < TAPS; t++){
        if ((reg >> t) & 1u) bus.gate_write(out[t], true);
    }
}

bool ShiftRegister::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_LENGTH:
            if (value == 0 || value > TAPS) return false;
            length = value;
            return true;
        case P_LOOP:
            if (value > 1) return false;
            loop = value != 0;
            return true;
        default:
            return false;
    }
}

uint8_t ShiftRegister::get_param(uint16_t index) const {
    switch (index){
        case P_LENGTH: return length;
        case P_LOOP:   return loop ? 1 : 0;
        default: return 0;
    }
}
