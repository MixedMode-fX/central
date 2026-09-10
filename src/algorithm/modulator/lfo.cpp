#include "algorithm/modulator/lfo.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::CV};

static const char* const SHAPE_NAMES[Lfo::LFO_SHAPES] = {
    "sine", "triangle", "ramp up", "ramp down", "square", "random step", "random glide",
};
static const char* const SYNC_NAMES[Lfo::LFO_SYNCS] = {"free", "clock"};
static const char* const POLARITY_NAMES[Lfo::LFO_POLARITIES] = {"bipolar", "unipolar"};

// Rate is in tenths of a hertz so that the slow end - a ten second cycle -
// and the fast end - 25.5 Hz, past where a modulation stops being heard as
// modulation - both fit one byte with useful steps in between.
static const ParamDescriptor PARAMS[9] = {
    {"shape",    Lfo::LFO_SINE,   Lfo::LFO_SHAPES, Lfo::LFO_SINE,    PARAM_ENUM,    SHAPE_NAMES},
    {"sync",     Lfo::LFO_FREE,   Lfo::LFO_SYNCS,  Lfo::LFO_FREE,    PARAM_ENUM,    SYNC_NAMES},
    {"rate",     1, 255, 20, PARAM_NUMBER,  nullptr},
    {"division", DIV_8_BARS,    DIVISIONS, DIV_BAR,       PARAM_ENUM, DIVISION_NAMES},
    {"feel",     FEEL_STRAIGHT, FEELS,     FEEL_STRAIGHT, PARAM_ENUM, FEEL_NAMES},
    {"depth",    1, 255, 255, PARAM_PERCENT, nullptr},
    {"offset",   0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"phase",    0, 255, 0,   PARAM_NUMBER,  nullptr},
    {"polarity", Lfo::LFO_BIPOLAR, Lfo::LFO_POLARITIES, Lfo::LFO_BIPOLAR, PARAM_ENUM, POLARITY_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, 9, PARAMS}};

static const char* const IN_NAMES[1] = {"reset"};
static const char* const OUT_NAMES[1] = {"cv"};

const AlgorithmDescriptor Lfo::descriptor = {
    ALGO_LFO, "LFO", 1, 0, 1, 9, IN, OUT, sizeof(Lfo), true, construct_node<Lfo>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A modulation source on a control bus: seven shapes, free-running or locked to the clock." };

// A quarter of a sine, 65 points at twelve bits, interpolated between. The
// other three quarters are this one reflected, so the table is 130 bytes and
// there is no floating point anywhere in the signal path.
static const uint16_t SINE_QUARTER[65] = {
       0,  100,  201,  301,  401,  501,  601,  700,
     799,  897,  995, 1092, 1189, 1285, 1380, 1474,
    1567, 1659, 1751, 1841, 1930, 2018, 2105, 2191,
    2275, 2358, 2439, 2519, 2598, 2675, 2750, 2824,
    2896, 2966, 3034, 3101, 3165, 3228, 3289, 3348,
    3405, 3460, 3512, 3563, 3611, 3658, 3702, 3744,
    3783, 3821, 3856, 3888, 3919, 3947, 3972, 3996,
    4016, 4035, 4051, 4064, 4075, 4084, 4090, 4094,
    4095,
};

// sin(x * pi/2 / 1024) at twelve bits, for x in 0 .. 1024.
static uint16_t sine_quarter(uint32_t x){
    if (x >= 1024u) return SINE_QUARTER[64];
    const uint32_t index = x >> 4;
    const uint32_t frac = x & 15u;
    const uint32_t a = SINE_QUARTER[index];
    const uint32_t b = SINE_QUARTER[index + 1];
    return (uint16_t)(a + ((b - a) * frac) / 16u);
}

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Lfo::Lfo(const NodeConfig& config) :
    reset_in(config.in_bus[0]),
    out(config.out_bus[0]),
    shape(clamp_enum(config.params[0], LFO_SHAPES, LFO_SINE)),
    sync(clamp_enum(config.params[1], LFO_SYNCS, LFO_FREE)),
    rate_param(config.params[2] ? config.params[2] : 20),
    div(clamp_enum(config.params[3], DIVISIONS, DIV_BAR)),
    how(clamp_enum(config.params[4], FEELS, FEEL_STRAIGHT)),
    depth(config.params[5] ? config.params[5] : 255),
    offset_param(config.params[6]),
    start_phase(config.params[7]),
    polarity(clamp_enum(config.params[8], LFO_POLARITIES, LFO_BIPOLAR)),
    sync_period(1), sync_origin(0), last_count(0),
    free_acc(0), free_inc(0), last_us(0),
    cycle_phase(0), random_from(0), random_to(0), last_value(0),
    rng(entropy::seed()), started(false), last_gate(false), have_time(false)
{
    derive();
    random_from = (uint16_t)(rng.next() & CV_MAX);
    random_to = (uint16_t)(rng.next() & CV_MAX);
    cycle_phase = (uint16_t)(((uint32_t)start_phase << 4) & CV_MAX);
}

void Lfo::derive(){
    sync_period = division_subticks(div, how);
    if (sync_period == 0) sync_period = 1;

    // Free-running phase is a 32-bit accumulator whose whole range is one
    // cycle: it wraps by itself, so a cycle boundary is "the accumulator went
    // backwards" rather than a comparison against a period, and a pass that
    // arrives late cannot lose a wrap.
    //
    // The increment is truncated to whole units per microsecond, which makes
    // the cycle up to 0.12% *long* at the slowest rate. That is an order of
    // magnitude finer than the rate control itself - one step of `rate` at
    // the slow end is a 100% change - so the truncation is invisible next to
    // the resolution of the thing being set.
    const uint32_t cycle_us = 10000000u / (uint32_t)(rate_param ? rate_param : 1u);
    free_inc = (uint32_t)(0x100000000ull / (uint64_t)(cycle_us ? cycle_us : 1u));
}

void Lfo::draw(){
    random_from = random_to;
    random_to = (uint16_t)(rng.next() & CV_MAX);
}

uint16_t Lfo::shape_at(uint16_t p) const {
    switch (shape){
        case LFO_TRIANGLE:
            // Bottom at the start of the cycle, top at the half, which is how
            // a triangle is drawn on every module that has one.
            return p < CV_HALF ? (uint16_t)(p * 2u)
                               : (uint16_t)(CV_FULL * 2u - 1u - (uint32_t)p * 2u);
        case LFO_RAMP_UP:
            return p;
        case LFO_RAMP_DOWN:
            return (uint16_t)(CV_MAX - p);
        case LFO_SQUARE:
            return p < CV_HALF ? (uint16_t)CV_MAX : (uint16_t)0;
        case LFO_RANDOM_STEP:
            return random_to;
        case LFO_RANDOM_GLIDE:
            return (uint16_t)((int32_t)random_from
                 + ((int32_t)random_to - (int32_t)random_from) * (int32_t)p / CV_FULL);
        default: {
            // Sine, from the quarter table. Starts at the centre going up, so
            // a bipolar sine leaves zero rising - the shape a musician draws
            // when they say "sine".
            const uint32_t quarter = (uint32_t)p >> 10;
            const uint32_t within = (uint32_t)p & 0x3FFu;
            int32_t s;
            switch (quarter){
                case 0:  s =  (int32_t)sine_quarter(within); break;
                case 1:  s =  (int32_t)sine_quarter(1024u - within); break;
                case 2:  s = -(int32_t)sine_quarter(within); break;
                default: s = -(int32_t)sine_quarter(1024u - within); break;
            }
            return (uint16_t)((s + CV_FULL) >> 1);
        }
    }
}

int16_t Lfo::scaled(uint16_t raw) const {
    // The offset is a signed byte read as a fraction of half of full scale,
    // so it can move a bipolar shape from one rail to the other.
    const int32_t offset = (int32_t)(int8_t)offset_param * CV_HALF / 128;
    if (polarity == LFO_UNIPOLAR){
        return (int16_t)cv_clamp_unipolar((int32_t)raw * depth / 255 + offset);
    }
    return (int16_t)cv_clamp_bipolar(((int32_t)raw - CV_HALF) * depth / 255 + offset);
}

void Lfo::restart(){
    free_acc = 0;
    sync_origin = last_count;
    started = true;
    cycle_phase = (uint16_t)(((uint32_t)start_phase << 4) & CV_MAX);
    draw();
}

void Lfo::process(BusManager& bus, uint32_t now_us){
    if (reset_in != NO_BUS){
        const bool level = bus.gate_read(reset_in);
        if (level && !last_gate) restart();
        last_gate = level;
    }

    if (sync == LFO_FREE){
        if (!have_time){
            have_time = true;
            last_us = now_us;
        }
        const uint32_t dt = (uint32_t)(now_us - last_us);
        last_us = now_us;
        const uint32_t before = free_acc;
        free_acc += dt * free_inc;
        if (free_acc < before) draw();          // the accumulator wrapped: a new cycle
        cycle_phase = (uint16_t)(((free_acc >> 20) + ((uint32_t)start_phase << 4)) & CV_MAX);
    }

    // Written once per pass, whichever mode produced the phase. Twice would
    // not be a mistake the bus hides: CV fan-in is a sum, so a second write
    // would double the modulation.
    last_value = scaled(shape_at(cycle_phase));
    bus.cv_write(out, last_value);
}

void Lfo::tick(BusManager&, uint32_t count){
    if (sync != LFO_CLOCK){
        last_count = count;                     // so a later reset has an anchor
        return;
    }
    if (!started || count < last_count){
        // Either the first tick this node has seen, or the clock restarted -
        // MasterClock::start() is the one place the count goes backwards.
        //
        // The cycle is anchored on **subtick zero**, not on whichever subtick
        // this node was constructed on: the downbeat is where the clock says
        // it is, so an LFO added to a patch that has been running for an hour
        // is in phase with the Metronome beside it and with any other LFO at
        // the same division. A reset edge is the one thing that moves the
        // anchor, which is what a reset inlet is for.
        started = true;
        sync_origin = count - (count % sync_period);
    }
    last_count = count;

    const uint32_t elapsed = count - sync_origin;
    const uint32_t cycles = elapsed / sync_period;
    if (cycles){
        // Whole cycles are skipped rather than looped, so an LFO added to a
        // patch that has been running for an hour costs the same as one
        // added at zero. One draw covers them: the levels a random shape
        // would have passed through were never on the bus.
        sync_origin += cycles * sync_period;
        draw();
    }
    const uint32_t within = count - sync_origin;
    cycle_phase = (uint16_t)((((within * CV_FULL) / sync_period)
                              + ((uint32_t)start_phase << 4)) & CV_MAX);
}

bool Lfo::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > LFO_SHAPES) return false;
            shape = value;
            return true;
        case 1:
            if (value == 0 || value > LFO_SYNCS) return false;
            if (value == sync) return true;
            sync = value;
            // Neither accumulator is cleared: switching a running LFO from
            // free to synced picks the cycle up where the clock says it is,
            // and switching back picks it up where the free accumulator had
            // got to. Restarting instead would put a step in the middle of
            // whatever it was modulating.
            return true;
        case 2:
            if (value == 0) return false;
            if (value == rate_param) return true;
            rate_param = value;
            break;
        case 3:
            if (value == 0 || value > DIVISIONS) return false;
            if (value == div) return true;
            div = value;
            break;
        case 4:
            if (value == 0 || value > FEELS) return false;
            if (value == how) return true;
            how = value;
            break;
        case 5:
            if (value == 0) return false;       // the descriptor's minimum is 1
            depth = value;
            return true;
        case 6:
            offset_param = value;
            return true;
        case 7:
            start_phase = value;
            return true;
        case 8:
            if (value == 0 || value > LFO_POLARITIES) return false;
            polarity = value;
            return true;
        default:
            return false;
    }
    derive();
    return true;
}

uint8_t Lfo::get_param(uint16_t index) const {
    switch (index){
        case 0: return shape;
        case 1: return sync;
        case 2: return rate_param;
        case 3: return div;
        case 4: return how;
        case 5: return depth;
        case 6: return offset_param;
        case 7: return start_phase;
        case 8: return polarity;
        default: return 0;
    }
}
