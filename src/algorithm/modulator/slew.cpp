#include "algorithm/modulator/slew.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::CV};
static const Domain OUT[1] = {Domain::CV};

static const ParamDescriptor PARAMS[3] = {
    {"rise", 0, 255, 0, PARAM_NUMBER, nullptr},
    {"fall", 0, 255, 0, PARAM_NUMBER, nullptr},
    {"link", 0, 1,   0, PARAM_BOOL,   nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 3, PARAMS}};

static const char* const IN_NAMES[1] = {"signal"};
static const char* const OUT_NAMES[1] = {"cv"};

const AlgorithmDescriptor Slew::descriptor = {
    ALGO_SLEW, "Slew", 1, 1, 1, 3, IN, OUT, sizeof(Slew), false, construct_node<Slew>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Limits how fast a control signal may change, up and down separately. Steps become glides." };

Slew::Slew(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    rise(config.params[0]),
    fall(config.params[1]),
    link(config.params[2]),
    level(0), last_us(0), have_time(false)
{}

int32_t Slew::step_for(uint8_t rate, uint32_t dt) const {
    // rate is tens of milliseconds for a full-scale move, so full scale takes
    // rate * 10000 microseconds and the distance covered in dt is
    // CV_FULL * dt / (rate * 10000), carried in sub-units.
    if (rate == 0) return (int32_t)CV_FULL << SUB_BITS;      // instant
    const uint32_t full_us = (uint32_t)rate * 10000u;
    // 64-bit for the one multiply, because the exact answer is worth more
    // here than the handful of cycles: this is a rate, and rounding it down
    // every pass is how a slow glide silently stops short.
    return (int32_t)((((uint64_t)CV_FULL << SUB_BITS) * dt) / full_us);
}

void Slew::process(BusManager& bus, uint32_t now_us){
    const int32_t target = ((int32_t)(in == NO_BUS ? 0 : bus.cv_read(in))) << SUB_BITS;

    if (!have_time){
        // The first pass takes the input as it is: a node that started at
        // zero and slewed up to where the signal already was would put a
        // ramp at the top of every patch load.
        have_time = true;
        last_us = now_us;
        level = target;
        bus.cv_write(out, (int16_t)(level >> SUB_BITS));
        return;
    }

    uint32_t dt = (uint32_t)(now_us - last_us);
    last_us = now_us;
    // A pass that arrived very late - a long SysEx transfer, a patch swap -
    // must not be turned into one enormous step that defeats the limit, and
    // must not overflow the arithmetic above. 65 ms is two orders of
    // magnitude longer than a pass and still far shorter than the slowest
    // rate's full travel.
    if (dt > 65000u) dt = 65000u;

    const int32_t distance = target - level;
    if (distance == 0){
        bus.cv_write(out, (int16_t)(level >> SUB_BITS));
        return;
    }

    const uint8_t rate = distance > 0 ? rise : (link ? rise : fall);
    const int32_t step = step_for(rate, dt);
    if (distance > 0) level += (step >= distance) ? distance : step;
    else              level -= (step >= -distance) ? -distance : step;

    bus.cv_write(out, (int16_t)(level >> SUB_BITS));
}

bool Slew::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: rise = value; return true;
        case 1: fall = value; return true;
        case 2:
            if (value > 1) return false;
            link = value;
            return true;
        default:
            return false;
    }
}

uint8_t Slew::get_param(uint16_t index) const {
    switch (index){
        case 0: return rise;
        case 1: return fall;
        case 2: return link;
        default: return 0;
    }
}
