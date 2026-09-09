#include "algorithm/midi/velocity_curve.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const CURVE_NAMES[4] = {"linear", "soft", "hard", "fixed"};
static const ParamDescriptor PARAMS[4] = {
    {"curve",  0, 3,   0,   PARAM_ENUM,    CURVE_NAMES},
    {"scale",  1, 255, 100, PARAM_PERCENT, nullptr},
    {"offset", 0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"fixed",  1, 127, 100, PARAM_NUMBER,  nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 4, PARAMS}};

const AlgorithmDescriptor VelocityCurve::descriptor = {
    ALGO_VELOCITY, "VelocityCurve", 1, 1, 1, 4, IN, OUT, sizeof(VelocityCurve), false, construct_node<VelocityCurve>,
    GROUPS, 1 };

// Velocity never changes a pitch and never drops a note, so this is the one
// modifier whose parameters cannot strand anything: a note-off carries
// velocity 0 through unchanged whatever the curve has become.
bool VelocityCurve::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: if (value > CURVE_FIXED) return false; curve = value; return true;
        case 1: scale = value ? value : 100; return true;
        case 2: offset = (int8_t)value; return true;
        case 3: fixed = value ? value : 100; return true;
        default: return false;
    }
}

uint8_t VelocityCurve::get_param(uint16_t index) const {
    switch (index){
        case 0: return curve;
        case 1: return scale;
        case 2: return (uint8_t)offset;
        case 3: return fixed;
        default: return 0;
    }
}

VelocityCurve::VelocityCurve(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    curve(config.params[0]),
    scale(config.params[1] ? config.params[1] : 100),
    offset((int8_t)config.params[2]),
    fixed(config.params[3] ? config.params[3] : 100)
{}

// Integer square root, for the soft curve. No floating point on a signal path.
static uint16_t isqrt(uint32_t value){
    uint32_t rest = value, result = 0, bit = 1u << 30;
    while (bit > rest) bit >>= 2;
    while (bit != 0){
        if (rest >= result + bit){ rest -= result + bit; result = (result >> 1) + bit; }
        else result >>= 1;
        bit >>= 2;
    }
    return (uint16_t)result;
}

uint8_t VelocityCurve::apply(uint8_t velocity) const {
    int32_t v = velocity;
    switch (curve){
        case CURVE_SOFT:  v = isqrt((uint32_t)velocity * 127u); break;   // concave
        case CURVE_HARD:  v = ((int32_t)velocity * velocity) / 127; break;
        case CURVE_FIXED: v = fixed; break;
        default: break;
    }
    v = (v * (int32_t)scale) / 100 + offset;
    if (v < 1) v = 1;
    if (v > 127) v = 127;
    return (uint8_t)v;
}

void VelocityCurve::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        MidiEvent e = bus.note_read(in, i);
        if (is_note_on(e)) e.data2 = apply(e.data2);
        bus.note_write(out, e);
    }
}
