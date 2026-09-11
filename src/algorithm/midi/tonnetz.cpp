#include "algorithm/midi/tonnetz.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const char* const CYCLE_NAMES[Tonnetz::TONNETZ_CYCLES] = {"LR", "PL", "PR", "free"};

// Which transform each cycle names on each half of its step. `free` draws
// instead, so it has no row.
static const uint8_t CYCLE_STEP[3][2] = {
    {Tonnetz::TRANSFORM_L, Tonnetz::TRANSFORM_R},   // LR: fifths
    {Tonnetz::TRANSFORM_P, Tonnetz::TRANSFORM_L},   // PL: major thirds
    {Tonnetz::TRANSFORM_P, Tonnetz::TRANSFORM_R},   // PR: minor thirds
};

static const ParamDescriptor PARAMS[Tonnetz::N_PARAMS] = {
    {"cycle",     Tonnetz::TONNETZ_LR, Tonnetz::TONNETZ_CYCLES, Tonnetz::TONNETZ_LR,
                  PARAM_ENUM, CYCLE_NAMES},
    {"deviation", 0, 100, 0, PARAM_PERCENT, nullptr},
    {"diatonic",  0, 1,   0, PARAM_BOOL,    nullptr},
    {"root",      0, 127, Tonnetz::DEFAULT_ROOT, PARAM_PITCH, nullptr},
    {"minor",     0, 1,   0, PARAM_BOOL,    nullptr},
    {"scale",     0, SCALE_COUNT - 1, 0, PARAM_ENUM, PARAM_SCALE_NAMES},
    {"velocity",  1, 127, Tonnetz::DEFAULT_VELOCITY, PARAM_NUMBER,  nullptr},
    {"channel",   1, 16,  1, PARAM_CHANNEL, nullptr},
    {"seed",      0, 255, 0, PARAM_NUMBER,  nullptr},
    {"key",       0, global_scale::KEY_MODES - 1, 0, PARAM_ENUM, PARAM_KEY_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, Tonnetz::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"advance", "reset"};
static const char* const OUT_NAMES[1] = {"triad out"};

const AlgorithmDescriptor Tonnetz::descriptor = {
    ALGO_TONNETZ, "Tonnetz", 2, 1, 1, Tonnetz::N_PARAMS, IN, OUT, sizeof(Tonnetz), false,
    construct_node<Tonnetz>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Chromatic triads where one voice moves a semitone: the circle of fifths in two dimensions.",
    CATEGORY_MIDI };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Tonnetz::Tonnetz(const NodeConfig& config) :
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    note_out(config.out_bus[0]),
    cycle(clamp_enum(config.params[P_CYCLE], TONNETZ_CYCLES, TONNETZ_LR)),
    deviation(config.params[P_DEVIATION] > 100 ? (uint8_t)100 : config.params[P_DEVIATION]),
    diatonic(config.params[P_DIATONIC] != 0),
    root(config.params[P_ROOT] ? config.params[P_ROOT] : DEFAULT_ROOT),
    minor(config.params[P_MINOR] != 0),
    scale(config.params[P_SCALE]),
    velocity(config.params[P_VELOCITY] ? config.params[P_VELOCITY] : DEFAULT_VELOCITY),
    channel(config.params[P_CHANNEL] ? config.params[P_CHANNEL] : (uint8_t)1),
    seed(config.params[P_SEED]),
    key(config.params[P_KEY] < global_scale::KEY_MODES ? config.params[P_KEY] : (uint8_t)0),
    current_root(0), current_minor(false), step(0), started(false), at_first(true),
    rng(config.params[P_SEED] ? (uint32_t)(config.params[P_SEED] * 2654435761u) : entropy::seed()),
    sounding()
{}

bool Tonnetz::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_CYCLE:
            if (value == 0 || value > TONNETZ_CYCLES) return false;
            cycle = value; return true;
        case P_DEVIATION:
            if (value > 100) return false;
            deviation = value; return true;
        case P_DIATONIC:
            if (value > 1) return false;
            diatonic = value != 0; return true;
        case P_ROOT:
            root = value ? value : DEFAULT_ROOT; return true;
        case P_MINOR:
            if (value > 1) return false;
            minor = value != 0; return true;
        case P_SCALE:
            if (value >= SCALE_COUNT) return false;
            scale = value; return true;
        case P_VELOCITY:
            if (value > 127) return false;
            velocity = value ? value : DEFAULT_VELOCITY; return true;
        case P_CHANNEL:
            if (value > 16) return false;
            channel = value ? value : (uint8_t)1; return true;
        case P_SEED:
            // The walk the *next* reset returns to. It does not redraw now: a
            // seed that changed the chord the moment it was typed would make
            // the parameter unusable while the patch is playing.
            seed = value; return true;
        case P_KEY:
            if (value >= global_scale::KEY_MODES) return false;
            key = value; return true;
        default: return false;
    }
}

uint8_t Tonnetz::get_param(uint16_t index) const {
    switch (index){
        case P_CYCLE:     return cycle;
        case P_DEVIATION: return deviation;
        case P_DIATONIC:  return diatonic ? 1u : 0u;
        case P_ROOT:      return root;
        case P_MINOR:     return minor ? 1u : 0u;
        case P_SCALE:     return scale;
        case P_VELOCITY:  return velocity;
        case P_CHANNEL:   return channel;
        case P_SEED:      return seed;
        case P_KEY:       return key;
        default: return 0;
    }
}

// The three transforms, as pure arithmetic on a root pitch class and a
// quality. Every one of them changes the quality - a major triad has no major
// neighbour on the Tonnetz - and moves exactly one of the three notes.
void Tonnetz::apply(uint8_t transform, uint8_t& root_pc, bool& is_minor){
    switch (transform){
        case TRANSFORM_P:
            // The third moves a semitone; the root and fifth stay.
            break;
        case TRANSFORM_L:
            // The root moves down a semitone: C major becomes E minor, and E
            // minor becomes C major.
            root_pc = is_minor ? (uint8_t)((root_pc + 8u) % 12u) : (uint8_t)((root_pc + 4u) % 12u);
            break;
        case TRANSFORM_R:
            // The fifth moves up a tone: C major becomes A minor, and A minor
            // becomes C major.
            root_pc = is_minor ? (uint8_t)((root_pc + 3u) % 12u) : (uint8_t)((root_pc + 9u) % 12u);
            break;
        default:
            return;
    }
    is_minor = !is_minor;
}

uint8_t Tonnetz::scheduled() const {
    if (cycle == TONNETZ_FREE) return 0xFF;
    return CYCLE_STEP[cycle - TONNETZ_LR][step & 1u];
}

uint8_t Tonnetz::active_root() const {
    return global_scale::resolve_tonic(key, root, DEFAULT_ROOT);
}

bool Tonnetz::in_key(uint8_t root_pc, bool is_minor) const {
    const uint16_t mask = global_scale::resolve_id(scale);
    const uint8_t tonic = (uint8_t)(active_root() % 12u);
    const uint8_t third = is_minor ? 3u : 4u;
    const uint8_t notes[3] = {root_pc,
                              (uint8_t)((root_pc + third) % 12u),
                              (uint8_t)((root_pc + 7u) % 12u)};
    for (uint8_t i = 0; i < 3; i++){
        const uint8_t degree = (uint8_t)(((int16_t)notes[i] - (int16_t)tonic + 12) % 12);
        if (!(mask & (uint16_t)(1u << degree))) return false;
    }
    return true;
}

uint8_t Tonnetz::choose(){
    uint8_t want = scheduled();
    if (want == 0xFF) want = rng.below(3);                    // `free`
    else if (deviation && rng.chance(deviation)){
        // One of the other two, uniformly: a deviation that could land on the
        // transform the cycle already named would not be one.
        want = (uint8_t)((want + 1u + rng.below(2)) % 3u);
    }
    if (!diatonic) return want;
    // P is never diatonic, so in practice this restricts the walk to L and R.
    for (uint8_t i = 0; i < 3; i++){
        const uint8_t candidate = (uint8_t)((want + i) % 3u);
        uint8_t candidate_root = current_root;
        bool candidate_minor = current_minor;
        apply(candidate, candidate_root, candidate_minor);
        if (in_key(candidate_root, candidate_minor)) return candidate;
    }
    return 0xFF;
}

void Tonnetz::strike(BusManager& bus){
    sounding.release_all(bus, note_out);
    // Root position, in the register the key names or `root` does: the walk
    // moves the pitch class, and the register stays where it was put.
    const uint8_t home = active_root();
    int16_t pitch = (int16_t)current_root;
    while (pitch + 12 <= (int16_t)home + 6) pitch += 12;
    while (pitch > (int16_t)home + 6) pitch -= 12;
    while (pitch < 0) pitch += 12;
    const int16_t third = current_minor ? 3 : 4;
    const int16_t notes[3] = {pitch, (int16_t)(pitch + third), (int16_t)(pitch + 7)};
    for (uint8_t i = 0; i < 3; i++){
        if (notes[i] < 0 || notes[i] > 127) continue;
        sounding.emit(bus, note_out, (uint8_t)pitch, (uint8_t)notes[i], velocity, channel);
    }
    started = true;
}

void Tonnetz::restart(){
    at_first = true;
    step = 0;
    started = false;
    // A seed is the walk the next reset returns to, which is what makes a
    // patch exactly reproducible when it names one.
    rng.reseed(seed ? (uint32_t)(seed * 2654435761u) : entropy::seed());
}

void Tonnetz::process(BusManager& bus, uint32_t){
    if (reset_in.rising(bus)) restart();
    if (!advance_in.rising(bus)) return;

    if (at_first){
        current_root = (uint8_t)(active_root() % 12u);
        current_minor = minor;
        at_first = false;
        strike(bus);
        return;
    }

    const uint8_t transform = choose();
    if (transform != 0xFF) apply(transform, current_root, current_minor);
    // The cycle advances whichever transform was taken, and even when
    // `diatonic` refused all three: it is a metre, not a queue.
    step ^= 1u;
    strike(bus);
}

void Tonnetz::silence(BusManager& bus){
    sounding.release_all(bus, note_out);
}
