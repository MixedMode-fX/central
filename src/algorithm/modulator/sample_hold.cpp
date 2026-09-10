#include "algorithm/modulator/sample_hold.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::CV};
static const Domain OUT[1] = {Domain::CV};

static const char* const SOURCE_NAMES[SampleHold::SH_SOURCES] = {"auto", "signal", "random"};
static const char* const MODE_NAMES[SampleHold::SH_MODES] = {"sample", "track"};

static const ParamDescriptor PARAMS[3] = {
    {"source", SampleHold::SH_AUTO,   SampleHold::SH_SOURCES, SampleHold::SH_AUTO,   PARAM_ENUM, SOURCE_NAMES},
    {"mode",   SampleHold::SH_SAMPLE, SampleHold::SH_MODES,   SampleHold::SH_SAMPLE, PARAM_ENUM, MODE_NAMES},
    {"steps",  0, 255, 0, PARAM_NUMBER, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 3, PARAMS}};

static const char* const IN_NAMES[2] = {"trigger", "signal"};
static const char* const OUT_NAMES[1] = {"cv"};

const AlgorithmDescriptor SampleHold::descriptor = {
    ALGO_SAMPLE_HOLD, "SampleHold", 2, 1, 1, 3, IN, OUT, sizeof(SampleHold), false, construct_node<SampleHold>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Holds one reading of a control signal until the next trigger. Unpatched, it holds noise.",
    CATEGORY_MODULATOR };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

SampleHold::SampleHold(const NodeConfig& config) :
    trigger_in(config.in_bus[0]),
    signal_in(config.in_bus[1]),
    out(config.out_bus[0]),
    source(clamp_enum(config.params[0], SH_SOURCES, SH_AUTO)),
    mode(clamp_enum(config.params[1], SH_MODES, SH_SAMPLE)),
    steps(config.params[2]),
    held(0), raw(0), sample_count(0), rng(entropy::seed()), last_gate(false)
{}

int16_t SampleHold::quantise(int16_t v) const {
    if (steps < 2) return v;
    // A grid of `steps` levels across full scale, rounded to the nearest - so
    // `steps` = 2 is "top or bottom" and not "top or nothing".
    //
    // The grid is centred on zero rather than laid out from zero upwards,
    // because the signal being held may be either: a bipolar LFO runs
    // -CV_HALF .. CV_HALF-1, and a grid that started at zero would flatten
    // its whole negative half onto one level - which is precisely the
    // "a slow LFO becomes a stepped sequence" patch this control is for.
    // Nothing is clamped for the same reason: a bus value is a sum, and a
    // reading larger than full scale is a real reading.
    const int32_t step = CV_MAX / ((int32_t)steps - 1);
    if (step <= 0) return v;
    const int32_t half = step / 2;
    const int32_t level = ((int32_t)v >= 0) ? ((int32_t)v + half) / step
                                            : ((int32_t)v - half) / step;
    return (int16_t)(level * step);
}

int16_t SampleHold::take(BusManager& bus){
    const bool from_signal = (source == SH_SIGNAL)
                          || (source == SH_AUTO && signal_in != NO_BUS);
    if (from_signal){
        return signal_in == NO_BUS ? (int16_t)0 : bus.cv_read(signal_in);
    }
    // Unipolar noise at the bus's own resolution: a random level, not a
    // random bit.
    return (int16_t)(rng.next() & CV_MAX);
}

void SampleHold::process(BusManager& bus, uint32_t){
    const bool level = (trigger_in == NO_BUS) ? false : bus.gate_read(trigger_in);

    if (mode == SH_TRACK){
        // Follow while the gate is up; whatever was there when it fell is
        // what is held.
        if (level){
            raw = take(bus);
            held = quantise(raw);
            if (!last_gate) sample_count++;
        }
    } else if (level && !last_gate){
        raw = take(bus);
        held = quantise(raw);
        sample_count++;
    }
    last_gate = level;

    bus.cv_write(out, held);
}

bool SampleHold::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > SH_SOURCES) return false;
            source = value;
            return true;
        case 1:
            if (value == 0 || value > SH_MODES) return false;
            mode = value;
            return true;
        case 2:
            if (value == steps) return true;
            steps = value;
            // Requantise what is already held, from the reading it came from,
            // so the control does something before the next trigger and so
            // coarsening then refining does not compound.
            held = quantise(raw);
            return true;
        default:
            return false;
    }
}

uint8_t SampleHold::get_param(uint16_t index) const {
    switch (index){
        case 0: return source;
        case 1: return mode;
        case 2: return steps;
        default: return 0;
    }
}
