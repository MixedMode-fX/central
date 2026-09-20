#include "algorithm/midi/velocity_curve.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const CURVE_NAMES[4] = {"linear", "soft", "hard", "fixed"};
static const ParamDescriptor PARAMS[VelocityCurve::N_PARAMS] = {
    {"curve",  0, 3,   0,   PARAM_ENUM,    CURVE_NAMES},
    {"scale",  1, 255, 100, PARAM_PERCENT, nullptr},
    {"offset", 0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"fixed",  1, 127, 100, PARAM_NUMBER,  nullptr},
    {"channel", 0, 16,   0, PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, VelocityCurve::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor VelocityCurve::descriptor = {
    ALGO_VELOCITY, "VelocityCurve", 1, 1, 1, VelocityCurve::N_PARAMS, IN, OUT,
    sizeof(VelocityCurve), false, construct_node<VelocityCurve>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Reshapes note-on velocity. Pitch is untouched and no note is ever dropped.",
    CATEGORY_MIDI };

// Velocity never changes a pitch and never drops a note, so none of the four
// velocity controls can strand anything: a note-off carries velocity 0
// through unchanged whatever the curve has become. `channel` is the one that
// could, and does not, because the note-off is sent where the table says the
// note-on went rather than where this byte now points.
bool VelocityCurve::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_CURVE: if (value > CURVE_FIXED) return false; curve = value; return true;
        case P_SCALE: scale = value ? value : 100; return true;
        case P_OFFSET: offset = (int8_t)value; return true;
        case P_FIXED: fixed = value ? value : 100; return true;
        case P_CHANNEL: if (value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t VelocityCurve::get_param(uint16_t index) const {
    switch (index){
        case P_CURVE:   return curve;
        case P_SCALE:   return scale;
        case P_OFFSET:  return (uint8_t)offset;
        case P_FIXED:   return fixed;
        case P_CHANNEL: return channel;
        default: return 0;
    }
}

VelocityCurve::VelocityCurve(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    curve(config.params[P_CURVE]),
    scale(config.params[P_SCALE] ? config.params[P_SCALE] : 100),
    offset((int8_t)config.params[P_OFFSET]),
    fixed(config.params[P_FIXED] ? config.params[P_FIXED] : 100),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    sent_on{}
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
        const uint8_t pitch = (uint8_t)(e.data1 & 0x7F);
        if (is_note_on(e)){
            e.data2 = apply(e.data2);
            e.channel = out_channel(channel, e.channel);
            sent_on[pitch] = e.channel;
        } else if (is_note_off(e)){
            // Where the note-on went, not where this node now points: a note
            // released on a channel it never sounded on never stops.
            e.channel = sent_on[pitch] ? sent_on[pitch] : out_channel(channel, e.channel);
            sent_on[pitch] = 0;
        } else {
            e.channel = out_channel(channel, e.channel);
        }
        bus.note_write(out, e);
    }
}
