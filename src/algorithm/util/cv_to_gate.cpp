#include "algorithm/util/cv_to_gate.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::CV};
static const Domain OUT[1] = {Domain::Gate};

static const char* const POLARITY_NAMES[CvToGate::CVG_POLARITIES] = {"bipolar", "unipolar"};
static const char* const MODE_NAMES[CvToGate::CVG_MODES] = {"gate", "trigger"};

static const ParamDescriptor PARAMS[6] = {
    {"threshold",  1, 100, 50, PARAM_PERCENT, nullptr},
    {"hysteresis", 0, 50,   2, PARAM_PERCENT, nullptr},
    {"polarity",   CvToGate::CVG_BIPOLAR, CvToGate::CVG_POLARITIES, CvToGate::CVG_BIPOLAR, PARAM_ENUM, POLARITY_NAMES},
    {"mode",       CvToGate::CVG_GATE,    CvToGate::CVG_MODES,      CvToGate::CVG_GATE,    PARAM_ENUM, MODE_NAMES},
    {"width",      0, 255, 0, PARAM_MILLIS, nullptr},
    {"invert",     0, 1,   0, PARAM_BOOL,   nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 6, PARAMS}};

static const char* const IN_NAMES[1] = {"cv"};
static const char* const OUT_NAMES[1] = {"gate"};

const AlgorithmDescriptor CvToGate::descriptor = {
    ALGO_CV_TO_GATE, "CvToGate", 1, 1, 1, 6, IN, OUT, sizeof(CvToGate), false,
    construct_node<CvToGate>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "The comparator: a control signal crossing a threshold becomes a gate.",
    CATEGORY_UTILITY };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

CvToGate::CvToGate(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    threshold(config.params[0] ? (config.params[0] > 100 ? 100 : config.params[0]) : (uint8_t)50),
    hysteresis(config.params[1] > 50 ? (uint8_t)50 : config.params[1]),
    polarity(clamp_enum(config.params[2], CVG_POLARITIES, CVG_BIPOLAR)),
    mode(clamp_enum(config.params[3], CVG_MODES, CVG_GATE)),
    width_param(config.params[4]),
    invert(config.params[5] ? 1 : 0),
    high(false), last_sense(false), count(0),
    pulse()
{
    if (config.params[4]) pulse.set_width_us((uint32_t)config.params[4] * 1000u);
}

void CvToGate::process(BusManager& bus, uint32_t now_us){
    const int16_t cv = bus.cv_read(in);
    int32_t level = (polarity == CVG_BIPOLAR) ? ((int32_t)cv + CV_HALF)
                                              : cv_clamp_unipolar((int32_t)cv);
    if (level < 0) level = 0;
    if (level > CV_MAX) level = CV_MAX;

    const int32_t up = ((int32_t)threshold * CV_FULL) / 100;
    int32_t down = up - ((int32_t)hysteresis * CV_FULL) / 100;
    if (down < 0) down = 0;

    if (!high){
        if (level >= up) high = true;
    } else if (level < down){
        high = false;
    }

    // Inverting the comparison rather than the output keeps the hysteresis on
    // the side the signal is actually resting against, and makes a trigger
    // fire on the crossing the user asked for rather than on the other one.
    const bool sense = invert ? !high : high;
    if (sense && !last_sense){
        count++;
        if (mode == CVG_TRIGGER) pulse.fire(now_us);
    }
    last_sense = sense;

    if (mode == CVG_TRIGGER) bus.gate_write(out, pulse.level(now_us));
    else bus.gate_write(out, sense);
}

bool CvToGate::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: if (value == 0 || value > 100) return false; threshold = value; return true;
        case 1: if (value > 50) return false; hysteresis = value; return true;
        case 2: if (value == 0 || value > CVG_POLARITIES) return false; polarity = value; return true;
        case 3: if (value == 0 || value > CVG_MODES) return false; mode = value; return true;
        case 4:
            width_param = value;
            pulse.set_width_us(value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US);
            return true;
        case 5: if (value > 1) return false; invert = value; return true;
        default: return false;
    }
}

uint8_t CvToGate::get_param(uint16_t index) const {
    switch (index){
        case 0: return threshold;
        case 1: return hysteresis;
        case 2: return polarity;
        case 3: return mode;
        case 4: return width_param;
        case 5: return invert;
        default: return 0;
    }
}
