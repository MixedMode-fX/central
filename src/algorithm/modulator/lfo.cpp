#include "algorithm/modulator/lfo.h"
#include "node/registry.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::CV};

static const char* const SYNC_NAMES[Lfo::LFO_SYNCS] = {"free", "clock"};
static const char* const POLARITY_NAMES[Lfo::LFO_POLARITIES] = {"bipolar", "unipolar"};

// Rate is in tenths of a hertz so that the slow end - a ten second cycle -
// and the fast end - 25.5 Hz, past where a modulation stops being heard as
// modulation - both fit one byte with useful steps in between.
static const ParamDescriptor PARAMS[9] = {
    {"shape",    Lfo::LFO_SINE,   Lfo::LFO_SHAPES, Lfo::LFO_SINE,    PARAM_ENUM,    CV_SHAPE_NAMES},
    {"sync",     Lfo::LFO_FREE,   Lfo::LFO_SYNCS,  Lfo::LFO_FREE,    PARAM_ENUM,    SYNC_NAMES},
    {"rate",     1, 255, 20, PARAM_NUMBER,  nullptr},
    {"division", DIV_8_BARS,    DIVISIONS, DIV_BAR,       PARAM_ENUM, DIVISION_NAMES},
    {"feel",     FEEL_STRAIGHT, FEELS,     FEEL_STRAIGHT, PARAM_ENUM, FEEL_NAMES},
    {"depth",    1, 255, 255, PARAM_PERCENT, nullptr},
    {"offset",   0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"phase",    0, 255, 0,   PARAM_NUMBER,  nullptr},
    {"polarity", Lfo::LFO_BIPOLAR, Lfo::LFO_POLARITIES, Lfo::LFO_BIPOLAR, PARAM_ENUM, POLARITY_NAMES},
};
// Three questions, in the order somebody setting up a modulator asks them:
// what shape is it, how fast does it go, and how far does it move. Sorted by
// name instead, `rate`, `division`, `feel` and `phase` all read as timing and
// land together under one heading - which puts the phase a reset starts at
// next to the note value the cycle is locked to, and leaves `sync` (the
// control that decides which of the two rate settings is live) in a different
// section from both of them.
//
// `shape` and `phase`/`polarity` are two groups sharing one label rather than
// one group, because the parameter order is the preset format and cannot be
// rearranged to make them adjacent. The editor folds groups with the same
// label into one section (app/src/views.js, paramSections).
static const ParamGroup GROUPS[4] = {
    {0, 1, 1, PARAMS,     "shape"},     // shape
    {1, 1, 4, PARAMS + 1, "rate"},      // sync, rate, division, feel
    {5, 1, 2, PARAMS + 5, "level"},     // depth, offset
    {7, 1, 2, PARAMS + 7, "shape"},     // phase, polarity
};

static const char* const IN_NAMES[1] = {"reset"};
static const char* const OUT_NAMES[1] = {"cv"};

const AlgorithmDescriptor Lfo::descriptor = {
    ALGO_LFO, "LFO", 1, 0, 1, 9, IN, OUT, sizeof(Lfo), true, construct_node<Lfo>,
    GROUPS, 4, IN_NAMES, OUT_NAMES,
    "A modulation source on a control bus: seven shapes, free-running or locked to the clock.",
    CATEGORY_MODULATOR };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Lfo::Lfo(const NodeConfig& config) :
    reset_in(config.in_buses[0]),
    out(config.out_buses[0]),
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

void Lfo::restart(){
    free_acc = 0;
    sync_origin = last_count;
    started = true;
    cycle_phase = (uint16_t)(((uint32_t)start_phase << 4) & CV_MAX);
    draw();
}

void Lfo::process(BusManager& bus, uint32_t now_us){
    if (reset_in.any()){
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
    last_value = cv_shape_scaled(cv_shape_at(shape, cycle_phase, random_from, random_to),
                                 depth, offset_param, polarity == LFO_UNIPOLAR);
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
