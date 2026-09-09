#include "algorithm/midi/velocity_curve.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor VelocityCurve::descriptor = {
    ALGO_VELOCITY, "VelocityCurve", 1, 1, 1, 4, IN, OUT, sizeof(VelocityCurve), false, construct_node<VelocityCurve> };

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
