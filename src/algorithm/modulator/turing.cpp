#include "algorithm/modulator/turing.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Gate, Domain::CV};

static const char* const WRITE_NAMES[Turing::TUR_WRITES] = {"follow", "clear", "fill"};
static const char* const POLARITY_NAMES[Turing::TUR_POLARITIES] = {"unipolar", "bipolar"};

static const ParamDescriptor PARAMS[7] = {
    {"length",   Turing::MIN_LENGTH, MAX_SEQUENCE_LEN, 8, PARAM_NUMBER, nullptr},
    {"chaos",    0, 100, 0, PARAM_PERCENT, nullptr},
    {"bits",     1, Turing::MAX_BITS, Turing::MAX_BITS, PARAM_NUMBER, nullptr},
    {"write",    Turing::TUR_FOLLOW,   Turing::TUR_WRITES,     Turing::TUR_FOLLOW,   PARAM_ENUM, WRITE_NAMES},
    {"seed",     0, 255, 0, PARAM_NUMBER, nullptr},
    {"width",    0, 255, 0, PARAM_MILLIS, nullptr},
    {"polarity", Turing::TUR_UNIPOLAR, Turing::TUR_POLARITIES, Turing::TUR_UNIPOLAR, PARAM_ENUM, POLARITY_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, 7, PARAMS}};

static const char* const IN_NAMES[2] = {"advance", "reset"};
static const char* const OUT_NAMES[2] = {"pulse", "cv"};

const AlgorithmDescriptor Turing::descriptor = {
    ALGO_TURING, "Turing", 2, 1, 2, 7, IN, OUT, sizeof(Turing), false,
    construct_node<Turing>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A looping shift register: chaos 0 locks the loop, 50 is noise, 100 inverts it.",
    CATEGORY_MODULATOR };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Turing::Turing(const NodeConfig& config) :
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    gate_out(config.out_bus[0]),
    cv_out(config.out_bus[1]),
    len(config.params[0] ? config.params[0] : (uint8_t)8),
    chaos(config.params[1] > 100 ? (uint8_t)100 : config.params[1]),
    bits(config.params[2] ? (config.params[2] > MAX_BITS ? MAX_BITS : config.params[2]) : MAX_BITS),
    write(clamp_enum(config.params[3], TUR_WRITES, TUR_FOLLOW)),
    seed(config.params[4]),
    width_param(config.params[5]),
    polarity(clamp_enum(config.params[6], TUR_POLARITIES, TUR_UNIPOLAR)),
    shift(0), count(0), level(0),
    // Seeded from entropy when `seed` is zero and from the byte otherwise:
    // a module must not play the same thing on every power cycle, and a
    // preset must play what it was saved with. RandomSequencer's rule.
    rng(config.params[4] ? (uint32_t)(config.params[4] * 2654435761u) : entropy::seed()),
    pulse()
{
    if (len < MIN_LENGTH) len = MIN_LENGTH;
    if (len > MAX_SEQUENCE_LEN) len = MAX_SEQUENCE_LEN;
    if (config.params[5]) pulse.set_width_us((uint32_t)config.params[5] * 1000u);
    draw();
}

void Turing::draw(){
    // A fresh stream every time, so reset really does return to the pattern
    // the node started with rather than to wherever the walk had wandered.
    rng.reseed(seed ? (uint32_t)(seed * 2654435761u) : entropy::seed());
    shift = rng.next();
    derive();
}

void Turing::derive(){
    const uint8_t width = bits > len ? len : bits;      // bits the ring does not have read as zero
    const uint32_t span = (uint32_t)((1u << width) - 1u);
    const uint32_t raw = shift & span;
    // Full scale across the bits that exist, so `bits` is a resolution
    // control and not also a range control: one bit is both rails, not the
    // bottom half of the range.
    int32_t v = span ? (int32_t)((raw * (uint32_t)CV_MAX) / span) : 0;
    if (polarity == TUR_BIPOLAR) v = cv_clamp_bipolar(v - CV_HALF);
    level = (int16_t)v;
}

void Turing::shift_once(){
    const uint32_t top = (shift >> (uint32_t)(len - 1u)) & 1u;
    uint32_t feed;
    switch (write){
        case TUR_CLEAR: feed = 0; break;
        case TUR_FILL:  feed = 1; break;
        default:
            // The one decision the node makes. At 0 the loop is exact; at 100
            // it is exactly inverted, which is a loop of twice the length;
            // at 50 the bit has no memory at all.
            feed = rng.chance(chaos) ? (top ^ 1u) : top;
            break;
    }
    shift = ((shift << 1) | feed) & mask();
    count++;
    derive();
}

void Turing::process(BusManager& bus, uint32_t now_us){
    if (reset_in.rising(bus)) draw();
    if (advance_in.rising(bus)){
        shift_once();
        if (shift & 1u) pulse.fire(now_us);
    }
    if (pulse.level(now_us)) bus.gate_write(gate_out, true);
    // The CV is a level, not an event: written every pass, whether or not the
    // register moved, because that is what a bus of int16_t already is.
    bus.cv_write(cv_out, level);
}

bool Turing::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: {
            if (value < MIN_LENGTH || value > MAX_SEQUENCE_LEN) return false;
            if (value == len) return true;
            // The low bits are kept, so shortening a loop leaves the steps
            // that have just played rather than a fresh pattern: the loop
            // gets shorter, it does not get replaced.
            len = value;
            shift &= mask();
            derive();
            return true;
        }
        case 1: if (value > 100) return false; chaos = value; return true;
        case 2:
            if (value == 0 || value > MAX_BITS) return false;
            if (value == bits) return true;
            bits = value;
            derive();                      // the level moves with the control
            return true;
        case 3: if (value == 0 || value > TUR_WRITES) return false; write = value; return true;
        case 4:
            // A new seed is the pattern the *next* reset returns to. It does
            // not redraw now: a seed that shredded the pattern the moment it
            // was touched would make the control unusable from a knob, which
            // is RandomSequencer's argument for the same parameter.
            seed = value;
            return true;
        case 5:
            width_param = value;
            pulse.set_width_us(value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US);
            return true;
        case 6:
            if (value == 0 || value > TUR_POLARITIES) return false;
            if (value == polarity) return true;
            polarity = value;
            derive();
            return true;
        default: return false;
    }
}

uint8_t Turing::get_param(uint16_t index) const {
    switch (index){
        case 0: return len;
        case 1: return chaos;
        case 2: return bits;
        case 3: return write;
        case 4: return seed;
        case 5: return width_param;
        case 6: return polarity;
        default: return 0;
    }
}
