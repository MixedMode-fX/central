#include "algorithm/modulator/envelope.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::CV, Domain::Gate};

static const char* const SYNC_NAMES[EnvelopeNode::ENV_SYNCS] = {"free", "clock"};
// Short because they have to be: these two sit in the narrowest column the
// editor has, and an option clipped to "alway" is worse than a terse word.
// What each one means is the enum's comment in envelope.h, which is where the
// registry's own description comes from.
static const char* const LOOP_NAMES[EnvelopeNode::ENV_LOOPS] = {"off", "held", "cycle"};
static const char* const RETRIG_NAMES[EnvelopeNode::ENV_RETRIGGERS] = {
    "zero", "level", "ignore",
};

// The curve bytes stop at +/- 100 rather than at the whole travel of a
// PARAM_CENTRED byte, so the control reads as a percentage of the way to the
// steepest curve the node draws and a mapped CC covers all of it.
#define ENV_CURVE_MIN (PARAM_CENTRE - 100)
#define ENV_CURVE_MAX (PARAM_CENTRE + 100)

// Parameters 0..11: the same in both envelopes, so they are described once
// and both descriptors' groups point at this array (node/param.h's
// ParamGroup carries a pointer to its fields, which is what makes that legal).
//
// Every stage carries a time *and* a note value, and `sync` says which of the
// two is live - the arrangement Lfo uses for `rate` and `division`, for the
// same reason: a descriptor is static, so one byte cannot be milliseconds on
// Tuesday and a note value on Wednesday. The editor greys out whichever is
// asleep (app/src/ui/panels/algorithms.js).
//
// The synced defaults are the note values that go with the free ones at a
// middling tempo, so switching `sync` on a fresh node changes the units and
// not the music.
static const ParamDescriptor COMMON[12] = {
    {"sync",         EnvelopeNode::ENV_FREE, EnvelopeNode::ENV_SYNCS,
                     EnvelopeNode::ENV_FREE, PARAM_ENUM, SYNC_NAMES},
    {"feel",         FEEL_STRAIGHT, FEELS, FEEL_STRAIGHT, PARAM_ENUM, FEEL_NAMES},

    {"delay",        1, 255, EnvelopeNode::OFF_TIME, PARAM_ENV_TIME, nullptr},
    {"delay div",    DIV_8_BARS, DIV_OFF, DIV_OFF, PARAM_ENUM, DIVISION_OFF_NAMES},

    {"attack",       1, 255, EnvelopeNode::DEFAULT_ATTACK, PARAM_ENV_TIME, nullptr},
    {"attack div",   DIV_8_BARS, DIV_OFF, DIV_32ND, PARAM_ENUM, DIVISION_OFF_NAMES},
    {"attack curve", ENV_CURVE_MIN, ENV_CURVE_MAX, PARAM_CENTRE, PARAM_CENTRED, nullptr},

    {"hold",         1, 255, EnvelopeNode::OFF_TIME, PARAM_ENV_TIME, nullptr},
    {"hold div",     DIV_8_BARS, DIV_OFF, DIV_OFF, PARAM_ENUM, DIVISION_OFF_NAMES},

    {"decay",        1, 255, EnvelopeNode::DEFAULT_DECAY, PARAM_ENV_TIME, nullptr},
    {"decay div",    DIV_8_BARS, DIV_OFF, DIV_EIGHTH, PARAM_ENUM, DIVISION_OFF_NAMES},
    {"decay curve",  ENV_CURVE_MIN, ENV_CURVE_MAX, PARAM_CENTRE, PARAM_CENTRED, nullptr},
};

// The four both envelopes end with, likewise described once.
//
// `level` is a percentage rather than the LFO's `depth` plus `offset`,
// because an envelope starts at rest and an offset would lift the rest as
// well as the peak - which is not a quieter envelope, it is a drone with a
// bump on it. What an inverted envelope needs instead is `invert`, and that
// is a switch.
static const ParamDescriptor OUTPUT[EnvelopeNode::O_COUNT] = {
    {"level",  1, 100, EnvelopeNode::DEFAULT_LEVEL, PARAM_PERCENT, nullptr},
    {"invert", 0, 1, 0, PARAM_BOOL, nullptr},
    {"loop",   EnvelopeNode::ENV_LOOP_OFF, EnvelopeNode::ENV_LOOPS,
               EnvelopeNode::ENV_LOOP_OFF, PARAM_ENUM, LOOP_NAMES},
    {"retrig", EnvelopeNode::ENV_RETRIG_ZERO, EnvelopeNode::ENV_RETRIGGERS,
               EnvelopeNode::ENV_RETRIG_ZERO, PARAM_ENUM, RETRIG_NAMES},
};

// What the ADSR has and the AD has not: somewhere to stop, and a fall from it.
static const ParamDescriptor SUSTAINING[4] = {
    {"sustain",       1, 100, EnvelopeNode::DEFAULT_SUSTAIN, PARAM_PERCENT, nullptr},
    {"release",       1, 255, EnvelopeNode::DEFAULT_RELEASE, PARAM_ENV_TIME, nullptr},
    {"release div",   DIV_8_BARS, DIV_OFF, DIV_QUARTER, PARAM_ENUM, DIVISION_OFF_NAMES},
    {"release curve", ENV_CURVE_MIN, ENV_CURVE_MAX, PARAM_CENTRE, PARAM_CENTRED, nullptr},
};

// One section per stage, in the order the contour walks them, so the editor
// draws an envelope as an envelope rather than as twenty knobs sorted by what
// their names sound like.
static const ParamGroup AD_GROUPS[6] = {
    {0,  1, 2, COMMON,      "clock"},
    {2,  1, 2, COMMON + 2,  "delay"},
    {4,  1, 3, COMMON + 4,  "attack"},
    {7,  1, 2, COMMON + 7,  "hold"},
    {9,  1, 3, COMMON + 9,  "decay"},
    {12, 1, 4, OUTPUT,      "output"},
};

static const ParamGroup ADSR_GROUPS[8] = {
    {0,  1, 2, COMMON,        "clock"},
    {2,  1, 2, COMMON + 2,    "delay"},
    {4,  1, 3, COMMON + 4,    "attack"},
    {7,  1, 2, COMMON + 7,    "hold"},
    {9,  1, 3, COMMON + 9,    "decay"},
    {12, 1, 1, SUSTAINING,    "sustain"},
    {13, 1, 3, SUSTAINING + 1,"release"},
    {16, 1, 4, OUTPUT,        "output"},
};

// Neither inlet is required. An envelope set to `cycle` is a shape
// generator that was never going to be triggered, and refusing to load it
// with nothing patched would be the validator arguing with the parameter -
// the same reason the LFO's reset is optional.
static const char* const AD_IN_NAMES[2] = {"trig", "reset"};
static const char* const ADSR_IN_NAMES[2] = {"gate", "retrig"};
static const char* const OUT_NAMES[2] = {"cv", "end"};

const AlgorithmDescriptor AdEnvelope::descriptor = {
    ALGO_AD, "AD", 2, 0, 2, AdEnvelope::N_PARAMS, IN, OUT, sizeof(AdEnvelope), true,
    construct_node<AdEnvelope>,
    AD_GROUPS, 6, AD_IN_NAMES, OUT_NAMES,
    "Delay, attack, hold, decay: an envelope a trigger fires and nothing stops. It loops too.",
    CATEGORY_MODULATOR };

const AlgorithmDescriptor AdsrEnvelope::descriptor = {
    ALGO_ADSR, "ADSR", 2, 0, 2, AdsrEnvelope::N_PARAMS, IN, OUT, sizeof(AdsrEnvelope), true,
    construct_node<AdsrEnvelope>,
    ADSR_GROUPS, 8, ADSR_IN_NAMES, OUT_NAMES,
    "An envelope as long as the note: it holds at the sustain level until the gate is let go.",
    CATEGORY_MODULATOR };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

static uint8_t clamp_percent(uint8_t stored, uint8_t fallback){
    if (stored == 0 || stored > 100) return fallback;
    return stored;
}

static uint8_t clamp_div(uint8_t stored, uint8_t fallback){
    if (stored < DIV_8_BARS || stored > DIV_OFF) return fallback;
    return stored;
}

static uint8_t clamp_curve(uint8_t stored){
    if (stored < ENV_CURVE_MIN || stored > ENV_CURVE_MAX) return (uint8_t)PARAM_CENTRE;
    return stored;
}

uint32_t env_curve(uint8_t amount, uint32_t frac){
    if (frac >= (uint32_t)CV_FULL) return (uint32_t)CV_FULL;
    const int32_t a = (int32_t)param_centred(amount);
    if (a == 0 || frac == 0) return frac;
    int32_t weight = a < 0 ? -a : a;
    if (weight > 100) weight = 100;
    // Both curves are one squaring, mirrored: `up` creeps and then runs,
    // `down` runs and then creeps. Neither ever leaves 0 .. CV_FULL and both
    // meet the straight line at each end, so blending towards one of them by
    // `weight` percent is still a walk from nothing to all of it.
    const uint32_t away = (uint32_t)CV_FULL - frac;
    const int32_t curved = a > 0
        ? (int32_t)((frac * frac) / (uint32_t)CV_FULL)
        : (int32_t)((uint32_t)CV_FULL - ((away * away) / (uint32_t)CV_FULL));
    return (uint32_t)((int32_t)frac + ((curved - (int32_t)frac) * weight) / 100);
}

EnvelopeNode::EnvelopeNode(const NodeConfig& config, bool sustaining, uint16_t output_first) :
    stage_time(), stage_div(), stage_curve(),
    sustain(0),
    level_pct(clamp_percent(config.params[output_first + O_LEVEL], DEFAULT_LEVEL)),
    invert(config.params[output_first + O_INVERT] ? 1u : 0u),
    loop(clamp_enum(config.params[output_first + O_LOOP], ENV_LOOPS, ENV_LOOP_OFF)),
    retrig(clamp_enum(config.params[output_first + O_RETRIG], ENV_RETRIGGERS, ENV_RETRIG_ZERO)),
    sync(clamp_enum(config.params[P_SYNC], ENV_SYNCS, ENV_FREE)),
    feel(clamp_enum(config.params[P_FEEL], FEELS, FEEL_STRAIGHT)),
    gate_in(config.in_buses[0]), aux_in(config.in_buses[1]),
    out(config.out_buses[0]), eoc_out(config.out_buses[1]),
    eoc(), elapsed(0), length(0), clock_count(0), taken_count(0), last_us(0),
    from_level(0), level(0), last_value(0), at(ENV_IDLE), has_sustain(sustaining),
    last_gate(false), last_aux(false), have_time(false), have_count(false)
{
    // A stored zero is the descriptor's default everywhere (node/param.h), and
    // a stage the patch never mentioned is the one the default has to be right
    // for: a delay and a hold nobody asked for are absent, an attack and a
    // decay nobody asked for are a pluck.
    stage_time[ENV_DELAY]   = config.params[P_DELAY]  ? config.params[P_DELAY]  : OFF_TIME;
    stage_time[ENV_ATTACK]  = config.params[P_ATTACK] ? config.params[P_ATTACK] : DEFAULT_ATTACK;
    stage_time[ENV_HOLD]    = config.params[P_HOLD]   ? config.params[P_HOLD]   : OFF_TIME;
    stage_time[ENV_DECAY]   = config.params[P_DECAY]  ? config.params[P_DECAY]  : DEFAULT_DECAY;
    stage_time[ENV_RELEASE] = DEFAULT_RELEASE;   // the ADSR's constructor knows better

    stage_div[ENV_DELAY]   = clamp_div(config.params[P_DELAY_DIV], DIV_OFF);
    stage_div[ENV_ATTACK]  = clamp_div(config.params[P_ATTACK_DIV], DIV_32ND);
    stage_div[ENV_HOLD]    = clamp_div(config.params[P_HOLD_DIV], DIV_OFF);
    stage_div[ENV_DECAY]   = clamp_div(config.params[P_DECAY_DIV], DIV_EIGHTH);
    stage_div[ENV_RELEASE] = DIV_QUARTER;

    stage_curve[ENV_ATTACK]  = clamp_curve(config.params[P_ATTACK_CURVE]);
    stage_curve[ENV_DECAY]   = clamp_curve(config.params[P_DECAY_CURVE]);
    stage_curve[ENV_RELEASE] = (uint8_t)PARAM_CENTRE;
    // The stages that only wait travel nowhere, so their curve is never read;
    // it is set anyway so that nothing here depends on that staying true.
    stage_curve[ENV_IDLE] = stage_curve[ENV_DELAY] = stage_curve[ENV_HOLD]
        = stage_curve[ENV_SUSTAIN] = (uint8_t)PARAM_CENTRE;

    // An envelope set to `cycle` is running before anything is played: that
    // is the whole point of it, and a patch loaded with one in it should be
    // cycling when the page draws it rather than on the first gate.
    if (loop == ENV_LOOP_FREE) enter(ENV_DELAY);
}

AdsrEnvelope::AdsrEnvelope(const NodeConfig& config) : EnvelopeNode(config, true, P_LEVEL) {
    sustain = clamp_percent(config.params[P_SUSTAIN], DEFAULT_SUSTAIN);
    stage_time[ENV_RELEASE] = config.params[P_RELEASE] ? config.params[P_RELEASE] : DEFAULT_RELEASE;
    stage_div[ENV_RELEASE] = clamp_div(config.params[P_RELEASE_DIV], DIV_QUARTER);
    stage_curve[ENV_RELEASE] = clamp_curve(config.params[P_RELEASE_CURVE]);
}

uint16_t EnvelopeNode::sustain_level() const {
    if (!has_sustain) return 0;
    return (uint16_t)(((uint32_t)CV_MAX * sustain) / 100u);
}

uint32_t EnvelopeNode::units_of(uint8_t which) const {
    if (which == ENV_IDLE || which == ENV_SUSTAIN) return 0;
    if (sync == ENV_CLOCK) return division_subticks(stage_div[which], feel);
    // OFF_TIME is one pass of the graph, which is as close to no stage at all
    // as a wall-clock length gets; treating it as none is what keeps a free
    // envelope's "no delay" exactly as instant as a synced one's "off".
    const uint8_t stored = stage_time[which];
    return stored <= OFF_TIME ? 0u : param_env_time_us(stored);
}

uint16_t EnvelopeNode::target_of(uint8_t which) const {
    switch (which){
        case ENV_ATTACK:  return (uint16_t)CV_MAX;
        // Both of these wait rather than travel, so where they are heading is
        // where they started. A hold's start is the peak because the attack
        // before it always lands there, even when it is instant.
        case ENV_DELAY:
        case ENV_HOLD:    return from_level;
        case ENV_DECAY:
        case ENV_SUSTAIN: return sustain_level();
        default:          return 0;      // release, idle
    }
}

uint16_t EnvelopeNode::level_at() const {
    const uint16_t to = target_of(at);
    if (length == 0) return to;
    uint32_t frac = (uint32_t)(((uint64_t)elapsed * (uint64_t)CV_FULL) / length);
    frac = env_curve(stage_curve[at], frac);
    return (uint16_t)((int32_t)from_level
                      + (((int32_t)to - (int32_t)from_level) * (int32_t)frac) / CV_FULL);
}

void EnvelopeNode::enter(uint8_t next){
    at = next;
    elapsed = 0;
    if (next == ENV_IDLE){
        level = 0;
        from_level = 0;
        length = 0;
        return;
    }
    from_level = level;
    length = units_of(next);
}

void EnvelopeNode::idle(){ enter(ENV_IDLE); }

void EnvelopeNode::fire(){
    if (retrig == ENV_RETRIG_IGNORE && at != ENV_IDLE) return;
    if (retrig == ENV_RETRIG_ZERO) level = 0;
    enter(ENV_DELAY);
}

uint8_t EnvelopeNode::next_stage(bool gate_high) const {
    switch (at){
        case ENV_DELAY:  return ENV_ATTACK;
        case ENV_ATTACK: return ENV_HOLD;
        case ENV_HOLD:   return ENV_DECAY;
        case ENV_DECAY:
            // The one decision in the walk. `cycle` is a shape generator and
            // never asks about the gate; `held` asks every time round, which
            // is what makes the loop stop when the note does rather than one
            // contour later.
            if (loop == ENV_LOOP_FREE) return ENV_DELAY;
            if (loop == ENV_LOOP_HELD) return gate_high ? ENV_DELAY
                                                        : (has_sustain ? ENV_RELEASE : ENV_IDLE);
            return has_sustain ? ENV_SUSTAIN : ENV_IDLE;
        case ENV_RELEASE: return loop == ENV_LOOP_FREE ? ENV_DELAY : ENV_IDLE;
        default:          return ENV_IDLE;
    }
}

void EnvelopeNode::retime(){
    const uint32_t was = length;
    length = units_of(at);
    // The fraction of the stage already travelled is what survives an edit,
    // not the time: an attack lengthened under a finger carries on from where
    // it had got to instead of jumping back down. Same rule as the LFO's
    // phase under a rate sweep.
    elapsed = (was == 0) ? 0u : (uint32_t)(((uint64_t)elapsed * length) / was);
}

void EnvelopeNode::process(BusManager& bus, uint32_t now_us){
    const bool gate_high = gate_in.any() && bus.gate_read(gate_in);
    const bool aux_high = aux_in.any() && bus.gate_read(aux_in);

    // The second inlet is the one thing the two envelopes do not share. On an
    // AD it cuts the contour, which is how a loop is stopped and a long fade
    // abandoned; on an ADSR it re-fires one under a gate that never fell.
    if (aux_high && !last_aux){
        if (has_sustain) fire();
        else idle();
    }
    last_aux = aux_high;

    if (gate_high && !last_gate){
        fire();
    } else if (!gate_high && last_gate && has_sustain
               && loop != ENV_LOOP_FREE && at != ENV_IDLE){
        // Released from wherever the contour stands - the sustain usually,
        // but the middle of a loop or of the attack just as well.
        enter(ENV_RELEASE);
    }
    last_gate = gate_high;

    if (loop == ENV_LOOP_FREE && at == ENV_IDLE) enter(ENV_DELAY);

    uint32_t dt;
    if (sync == ENV_CLOCK){
        dt = (uint32_t)(clock_count - taken_count);
        taken_count = clock_count;
    } else {
        if (!have_time){
            have_time = true;
            last_us = now_us;
        }
        dt = (uint32_t)(now_us - last_us);
        last_us = now_us;
        // The guard Slew uses, for the same reason: a pass that arrived very
        // late - a long SysEx transfer, a patch swap - must not be turned
        // into one step that skips a whole contour.
        if (dt > 65000u) dt = 65000u;
    }

    bool completed = false;
    // Two stages per hop's worth of headroom: one pass may legitimately cross
    // a whole contour of stages set to nothing and wrap once. Past that the
    // envelope is every stage at zero and looping, which has no end to walk
    // to, so the walk stops and the next pass takes it up again.
    for (uint8_t hops = 0; hops < 2u * ENV_STAGES; hops++){
        if (at == ENV_IDLE){ level = 0; break; }
        if (at == ENV_SUSTAIN){ elapsed = 0; level = sustain_level(); break; }
        if (length > elapsed){
            const uint32_t remaining = length - elapsed;
            if (dt < remaining){
                elapsed += dt;
                level = level_at();
                break;
            }
            dt -= remaining;
        }
        level = target_of(at);
        const uint8_t next = next_stage(gate_high);
        // Idle and a wrap back to the delay are the only two ways out of the
        // last stage, so between them they are "the contour finished" - which
        // is what the end outlet fires on, loop or no loop.
        if (next == ENV_IDLE || next == ENV_DELAY) completed = true;
        enter(next);
    }

    if (completed) eoc.fire(now_us);
    bus.gate_write(eoc_out, eoc.level(now_us));

    int32_t scaled = ((int32_t)level * (int32_t)level_pct) / 100;
    if (scaled > CV_MAX) scaled = CV_MAX;
    last_value = (int16_t)(invert ? -scaled : scaled);
    bus.cv_write(out, last_value);
}

void EnvelopeNode::tick(BusManager&, uint32_t count){
    // A free-running envelope keeps the count up to date without spending it,
    // so switching `sync` on does not hand the walk an hour of subticks. The
    // backwards test is MasterClock::start(), the one place the count resets.
    if (!have_count || count < taken_count || sync != ENV_CLOCK){
        have_count = true;
        taken_count = count;
    }
    clock_count = count;
}

bool EnvelopeNode::set_output(uint16_t which, uint8_t value){
    switch (which){
        case O_LEVEL:
            if (value == 0 || value > 100) return false;
            level_pct = value;
            return true;
        case O_INVERT:
            invert = value ? 1u : 0u;
            return true;
        case O_LOOP:
            if (value == 0 || value > ENV_LOOPS) return false;
            loop = value;
            return true;
        case O_RETRIG:
            if (value == 0 || value > ENV_RETRIGGERS) return false;
            retrig = value;
            return true;
        default:
            return false;
    }
}

uint8_t EnvelopeNode::get_output(uint16_t which) const {
    switch (which){
        case O_LEVEL:  return level_pct;
        case O_INVERT: return invert;
        case O_LOOP:   return loop;
        case O_RETRIG: return retrig;
        default:       return 0;
    }
}

bool EnvelopeNode::set_param(uint16_t index, uint8_t value){
    // Every write that can change how long a stage is ends in retime(), which
    // is a no-op unless the stage it changed is the one running.
    switch (index){
        case P_SYNC:
            if (value == 0 || value > ENV_SYNCS) return false;
            if (value == sync) return true;
            sync = value;
            // The walk counts in a different unit from this pass on, so the
            // count it would otherwise spend at once is dropped first.
            taken_count = clock_count;
            retime();
            return true;
        case P_FEEL:
            if (value == 0 || value > FEELS) return false;
            if (value == feel) return true;
            feel = value;
            retime();
            return true;
        case P_DELAY:  case P_ATTACK: case P_HOLD: case P_DECAY: {
            if (value == 0) return false;
            const uint8_t which = (index == P_DELAY) ? ENV_DELAY
                                : (index == P_ATTACK) ? ENV_ATTACK
                                : (index == P_HOLD) ? ENV_HOLD : ENV_DECAY;
            stage_time[which] = value;
            retime();
            return true;
        }
        case P_DELAY_DIV: case P_ATTACK_DIV: case P_HOLD_DIV: case P_DECAY_DIV: {
            if (value < DIV_8_BARS || value > DIV_OFF) return false;
            const uint8_t which = (index == P_DELAY_DIV) ? ENV_DELAY
                                : (index == P_ATTACK_DIV) ? ENV_ATTACK
                                : (index == P_HOLD_DIV) ? ENV_HOLD : ENV_DECAY;
            stage_div[which] = value;
            retime();
            return true;
        }
        case P_ATTACK_CURVE:
            if (value < ENV_CURVE_MIN || value > ENV_CURVE_MAX) return false;
            stage_curve[ENV_ATTACK] = value;
            return true;
        case P_DECAY_CURVE:
            if (value < ENV_CURVE_MIN || value > ENV_CURVE_MAX) return false;
            stage_curve[ENV_DECAY] = value;
            return true;
        default:
            return index >= P_TAIL && set_tail((uint16_t)(index - P_TAIL), value);
    }
}

uint8_t EnvelopeNode::get_param(uint16_t index) const {
    switch (index){
        case P_SYNC:         return sync;
        case P_FEEL:         return feel;
        case P_DELAY:        return stage_time[ENV_DELAY];
        case P_DELAY_DIV:    return stage_div[ENV_DELAY];
        case P_ATTACK:       return stage_time[ENV_ATTACK];
        case P_ATTACK_DIV:   return stage_div[ENV_ATTACK];
        case P_ATTACK_CURVE: return stage_curve[ENV_ATTACK];
        case P_HOLD:         return stage_time[ENV_HOLD];
        case P_HOLD_DIV:     return stage_div[ENV_HOLD];
        case P_DECAY:        return stage_time[ENV_DECAY];
        case P_DECAY_DIV:    return stage_div[ENV_DECAY];
        case P_DECAY_CURVE:  return stage_curve[ENV_DECAY];
        default:             return index >= P_TAIL ? get_tail((uint16_t)(index - P_TAIL)) : 0;
    }
}

// The ADSR's tail: its own four, then the run both share.
bool AdsrEnvelope::set_tail(uint16_t index, uint8_t value){
    switch (index){
        case P_SUSTAIN - P_TAIL:
            if (value == 0 || value > 100) return false;
            sustain = value;
            return true;
        case P_RELEASE - P_TAIL:
            if (value == 0) return false;
            stage_time[ENV_RELEASE] = value;
            retime();
            return true;
        case P_RELEASE_DIV - P_TAIL:
            if (value < DIV_8_BARS || value > DIV_OFF) return false;
            stage_div[ENV_RELEASE] = value;
            retime();
            return true;
        case P_RELEASE_CURVE - P_TAIL:
            if (value < ENV_CURVE_MIN || value > ENV_CURVE_MAX) return false;
            stage_curve[ENV_RELEASE] = value;
            return true;
        default:
            return index >= (P_LEVEL - P_TAIL)
                && set_output((uint16_t)(index - (P_LEVEL - P_TAIL)), value);
    }
}

uint8_t AdsrEnvelope::get_tail(uint16_t index) const {
    switch (index){
        case P_SUSTAIN - P_TAIL:       return sustain;
        case P_RELEASE - P_TAIL:       return stage_time[ENV_RELEASE];
        case P_RELEASE_DIV - P_TAIL:   return stage_div[ENV_RELEASE];
        case P_RELEASE_CURVE - P_TAIL: return stage_curve[ENV_RELEASE];
        default:
            return index >= (P_LEVEL - P_TAIL)
                 ? get_output((uint16_t)(index - (P_LEVEL - P_TAIL))) : 0;
    }
}
