#include "algorithm/midi/harmony.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "hal/midi_types.h"
#include "midi/root_motion.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Note, Domain::CV};

static const ParamDescriptor PARAMS[Harmony::N_PARAMS] = {
    {"phrase",   2, Harmony::MAX_PHRASE, 4, PARAM_NUMBER,  nullptr},
    {"cadence",  0, 100,              75, PARAM_PERCENT, nullptr},
    {"gravity",  0, 100,               0, PARAM_PERCENT, nullptr},
    {"loop",     0, 1,                 0, PARAM_BOOL,    nullptr},
    {"root",     0, 127, Harmony::DEFAULT_ROOT, PARAM_PITCH, nullptr},
    {"scale",    0, SCALE_COUNT - 1,   0, PARAM_ENUM,    PARAM_SCALE_NAMES},
    {"velocity", 1, 127,             100, PARAM_NUMBER,  nullptr},
    {"channel",  1, 16,                1, PARAM_CHANNEL, nullptr},
    {"seed",     0, 255,               0, PARAM_NUMBER,  nullptr},
    // The walk itself. Each is a middle by default, so an unconfigured node
    // sounds like music and has room to move in both directions.
    {"fifths",   1, 100, Harmony::DEFAULT_FIFTHS,  PARAM_PERCENT, nullptr},
    {"smooth",   0, 100,                       0, PARAM_PERCENT, nullptr},
    {"leading",  1, 100, Harmony::DEFAULT_LEADING, PARAM_PERCENT, nullptr},
    {"spread",   1, 100, Harmony::DEFAULT_SPREAD,  PARAM_PERCENT, nullptr},
    {"drift",    0, 100,                       0, PARAM_PERCENT, nullptr},
    {"key",      0, global_scale::KEY_MODES - 1, 0, PARAM_ENUM, PARAM_KEY_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, Harmony::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"advance", "reset"};
static const char* const OUT_NAMES[2] = {"root", "degree"};

const AlgorithmDescriptor Harmony::descriptor = {
    ALGO_HARMONY, "Harmony", 2, 1, 2, Harmony::N_PARAMS, IN, OUT, sizeof(Harmony), false,
    construct_node<Harmony>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A chord progression in the module's key: weighs every move against the scale, plays the root.",
    CATEGORY_MIDI };


Harmony::Harmony(const NodeConfig& config) :
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    note_out(config.out_bus[0]),
    cv_out(config.out_bus[1]),
    phrase(config.params[P_PHRASE] ? (config.params[P_PHRASE] > MAX_PHRASE ? MAX_PHRASE
                                                                          : config.params[P_PHRASE])
                                   : (uint8_t)4),
    cadence(config.params[P_CADENCE] ? (config.params[P_CADENCE] > 100 ? (uint8_t)100
                                                                      : config.params[P_CADENCE])
                                     : (uint8_t)75),
    gravity(config.params[P_GRAVITY] > 100 ? (uint8_t)100 : config.params[P_GRAVITY]),
    loop(config.params[P_LOOP] ? 1 : 0),
    root(config.params[P_ROOT] ? (uint8_t)(config.params[P_ROOT] & 0x7F) : DEFAULT_ROOT),
    scale(config.params[P_SCALE] < SCALE_COUNT ? config.params[P_SCALE] : (uint8_t)0),
    velocity(config.params[P_VELOCITY] ? (uint8_t)(config.params[P_VELOCITY] & 0x7F) : (uint8_t)100),
    channel(config.params[P_CHANNEL] ? config.params[P_CHANNEL] : (uint8_t)1),
    seed(config.params[P_SEED]),
    fifths(config.params[P_FIFTHS] ? (config.params[P_FIFTHS] > 100 ? (uint8_t)100
                                                                    : config.params[P_FIFTHS])
                                   : DEFAULT_FIFTHS),
    smooth(config.params[P_SMOOTH] > 100 ? (uint8_t)100 : config.params[P_SMOOTH]),
    leading(config.params[P_LEADING] ? (config.params[P_LEADING] > 100 ? (uint8_t)100
                                                                      : config.params[P_LEADING])
                                     : DEFAULT_LEADING),
    spread(config.params[P_SPREAD] ? (config.params[P_SPREAD] > 100 ? (uint8_t)100
                                                                   : config.params[P_SPREAD])
                                   : DEFAULT_SPREAD),
    drift(config.params[P_DRIFT] > 100 ? (uint8_t)100 : config.params[P_DRIFT]),
    key(config.params[P_KEY] < global_scale::KEY_MODES ? config.params[P_KEY] : (uint8_t)0),
    current(0), position(0), recorded(0), started(false), at_first(true), written(),
    rng(config.params[P_SEED] ? (uint32_t)(config.params[P_SEED] * 2654435761u) : entropy::seed()),
    sounding()
{
    if (phrase < 2) phrase = 2;
}

uint8_t Harmony::usable_degrees() const {
    const uint8_t n = scale_size(global_scale::resolve_id(scale));
    return n < DEGREES ? n : DEGREES;
}

// Roots fold rather than disappear. A degree that would leave 0..127 is the
// same pitch class an octave the other way, and for a *root* that is the
// right answer - unlike a melody note, where Transpose and CvToNote drop it,
// because there a folded note is a wrong note and a missing one is a rest.
uint8_t Harmony::pitch_of(uint8_t deg) const {
    const uint16_t mask = global_scale::resolve_id(scale);
    const uint8_t n = usable_degrees();
    if (deg >= n && n) deg = (uint8_t)(n - 1u);
    int16_t pitch = (int16_t)global_scale::resolve_tonic(key, root, DEFAULT_ROOT)
                  + scale_degree_to_semitone((int16_t)deg, mask);
    while (pitch > 127) pitch -= 12;
    while (pitch < 0) pitch += 12;
    return (uint8_t)pitch;
}

// Every move weighed against the scale, then shaped. Split out of choose()
// because it is also what proves the walk: `likeliest_from` reads the same
// numbers with none of the randomness on top.
uint8_t Harmony::weigh(uint8_t from, uint32_t* weight) const {
    const uint16_t mask = global_scale::resolve_id(scale);
    const uint8_t n = usable_degrees();
    for (uint8_t j = 0; j < DEGREES; j++) weight[j] = 0;
    if (n <= 1) return n;
    if (from >= n) from = 0;

    const root_motion::Degrees d = root_motion::degrees_of(mask, n);
    // A mode with no semitone below the tonic has no cadence to make and no
    // leading tone to avoid, so `leading` has nothing to weight and says so
    // by doing nothing.
    const bool has_leading_tone = (mask & (uint16_t)(1u << 11)) != 0;

    uint32_t top = 0;
    for (uint8_t j = 0; j < n; j++){
        if (j == from){ weight[j] = 0; continue; }   // a repeat is `gravity`'s to give
        const uint8_t semis = root_motion::interval(d, from, j);
        // `smooth` crossfades between two theories of what makes one chord
        // follow another: how far the root moved, and how much the two chords
        // share. At 0 it is the interval alone - fifths lead. At 100 it is
        // the shared tones alone, and the mediants lead, which is what
        // Romantic harmony sounds like. It is a crossfade and not a bonus
        // because a bonus can only ever nudge: a fifth outweighs a third
        // three to one, and no amount of added weight was going to turn that
        // over.
        uint32_t w = root_motion::strength(semis);
        if (smooth){
            const uint32_t shared = root_motion::shared_strength(root_motion::common_tones(d, from, j));
            w = (w * (100u - smooth) + shared * smooth) / 100u;
        }

        // Which way round the circle. The far ends of `fifths` bias heavily
        // and never to zero: a move the walk can never make is a move that
        // can never surprise, and this node is for the ones that do.
        const root_motion::Direction dir = root_motion::direction(semis);
        uint32_t bias = 50;
        if (dir == root_motion::MOTION_FALLING) bias = 20u + (uint32_t)fifths * 80u / 100u;
        else if (dir == root_motion::MOTION_RISING) bias = 20u + (uint32_t)(100u - fifths) * 80u / 100u;
        w *= bias;

        if (has_leading_tone && root_motion::carries_leading_tone(d, j)){
            // 1 is a fifth of the weight, 50 leaves it alone, 100 nearly
            // doubles it: modal at one end, cadential at the other.
            w = w * (20u + (uint32_t)leading * 160u / 100u) / 100u;
        }

        weight[j] = w;
        if (w > top) top = w;
    }
    if (top == 0){ weight[0] = WEIGHT_SCALE; return n; }

    // Normalised before shaping, so `spread` and `gravity` mean the same
    // thing whatever the scale made of the raw numbers.
    uint32_t sum = 0;
    for (uint8_t j = 0; j < n; j++){
        weight[j] = weight[j] * WEIGHT_SCALE / top;
        sum += weight[j];
    }

    // `spread` is the whole difference between a progression and a process.
    // Below the midpoint it cubes the weights, so the likeliest move gets
    // likelier and the walk hardens toward a loop; above it, it mixes toward
    // every legal move being equally likely. At 50 they are played as
    // computed, and at 100 it is the uniform walk exactly.
    const uint32_t mean = sum / n;
    for (uint8_t j = 0; j < n; j++){
        const uint32_t w = weight[j];
        if (spread < 50){
            const uint32_t sharp = (w * w / WEIGHT_SCALE) * w / WEIGHT_SCALE;
            const uint32_t t = (uint32_t)(50u - spread);
            weight[j] = (sharp * t + w * (50u - t)) / 50u;
        } else if (spread > 50){
            const uint32_t t = (uint32_t)(spread - 50);
            weight[j] = (mean * t + w * (50u - t)) / 50u;
        }
    }

    // `gravity` mixes an increasing weight on the tonic into whatever the
    // walk wanted, so 100 never leaves home whatever else is set. The walk's
    // own weights are multiplied out rather than divided down, because
    // dividing would round the rarest moves to nothing long before gravity
    // reached the top - which is the opposite of a gravity control, whose job
    // is to shorten every move evenly.
    if (gravity){
        for (uint8_t j = 0; j < n; j++) weight[j] = weight[j] * (100u - gravity) / 100u;
        weight[0] += (uint32_t)gravity * WEIGHT_SCALE / 100u;
    }
    return n;
}

uint8_t Harmony::likeliest_from(uint8_t from) const {
    uint32_t weight[DEGREES];
    const uint8_t n = weigh(from, weight);
    uint8_t best = 0;
    for (uint8_t j = 1; j < n; j++) if (weight[j] > weight[best]) best = j;
    return best;
}

uint8_t Harmony::choose(){
    const uint8_t n = usable_degrees();
    if (n <= 1) return 0;

    // The cadence: the phrase's last chord resolves. This is the one thing
    // that turns a walk into a period, and it is checked before the weights
    // so that nothing about the walk can talk it out of a resolution.
    if (position + 1u == phrase && rng.chance(cadence)) return 0;

    uint32_t weight[DEGREES];
    weigh(current, weight);
    uint32_t total = 0;
    for (uint8_t j = 0; j < n; j++) total += weight[j];
    if (total == 0) return 0;

    // Xorshift32::below is bias-free and takes a byte; these weights run into
    // the thousands, where the modulo bias its comment warns about is one part
    // in four hundred thousand of one draw. That is smaller than the
    // difference between any two weights the walk produces.
    uint32_t pick = rng.next() % total;
    for (uint8_t j = 0; j < n; j++){
        if (pick < weight[j]) return j;
        pick -= weight[j];
    }
    return 0;
}

void Harmony::strike(BusManager& bus, uint8_t deg){
    current = deg;
    const uint8_t pitch = pitch_of(deg);
    sounding.release_all(bus, note_out);
    // One voice, no incoming note: every emission shares the ledger's source
    // key, so release_all is the only release this node needs.
    sounding.emit(bus, note_out, 0, pitch, velocity, channel);
    started = true;
}

void Harmony::restart(){
    // What reset means in every sequencer here: the next advance plays the
    // first step. It does not silence what is sounding - a root that dropped
    // out between the reset and the next chord would be a hole in the music,
    // and the next advance replaces it anyway.
    position = 0;
    at_first = true;
}

void Harmony::process(BusManager& bus, uint32_t){
    if (reset_in.rising(bus)) restart();

    if (advance_in.rising(bus)){
        uint8_t deg;
        if (loop && recorded >= phrase){
            deg = written[position];               // the phrase, as written
            // An accident that happens once is a glitch and one that comes
            // back is a decision, so a redraw *replaces* the chord it landed
            // on. A looping phrase with a few percent of drift is a piece
            // that is recognisably itself and never quite the same twice.
            if (drift && rng.chance(drift)){
                deg = choose();
                written[position] = deg;
            }
        } else if (at_first){
            deg = 0;                               // the first advance is the tonic
        } else {
            deg = choose();
        }
        at_first = false;

        // Recording only ever starts at the top of a phrase, so switching
        // `loop` on halfway through waits for the next one rather than
        // capturing a phrase that begins in the middle.
        if (loop && recorded < phrase && recorded == position){
            written[position] = deg;
            recorded = (uint8_t)(position + 1u);
        }

        strike(bus, deg);
        position = (uint8_t)((position + 1u) % phrase);
    } else if (sounding.count()){
        // Re-voiced when what it should be playing changes - the key moving
        // under it, or the root or the scale being edited. Chord does exactly
        // this and for the same reason: a held note that cannot follow the
        // key drags the whole patch out of it, and the release still comes
        // from the ledger, so re-voicing cannot strand anything.
        //
        // Re-voicing what is sounding, never starting it: this used to strike
        // whenever the ledger was empty too, which meant a node told to stand
        // down - the transport stopping (node/node.h) - played again on the
        // very next pass. Nothing is being re-voiced when nothing is
        // sounding; the next advance is what plays.
        const uint8_t want = pitch_of(current);
        if (sounding.at(0).note != want) strike(bus, current);
    }

    if (cv_out != NO_BUS){
        const uint8_t n = usable_degrees();
        const int32_t span = n > 1 ? (int32_t)(n - 1u) : 1;
        bus.cv_write(cv_out, (int16_t)(((int32_t)current * CV_MAX) / span));
    }
}

void Harmony::silence(BusManager& bus){
    sounding.release_all(bus, note_out);
}

bool Harmony::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_PHRASE:
            if (value < 2 || value > MAX_PHRASE) return false;
            if (value == phrase) return true;
            // A phrase of a different length is a different phrase, so the
            // one that was recorded is no longer the piece.
            phrase = value;
            recorded = 0;
            if (position >= phrase) position = 0;
            return true;
        case P_CADENCE: if (value > 100) return false; cadence = value; return true;
        case P_GRAVITY: if (value > 100) return false; gravity = value; return true;
        case P_LOOP:
            if (value > 1) return false;
            if (value == loop) return true;
            loop = value;
            recorded = 0;          // on: record the next phrase. off: walk again.
            return true;
        case P_ROOT: if (value > 127) return false; root = value; return true;
        case P_SCALE: if (value >= SCALE_COUNT) return false; scale = value; return true;
        case P_VELOCITY: if (value == 0 || value > 127) return false; velocity = value; return true;
        case P_CHANNEL: if (value == 0 || value > 16) return false; channel = value; return true;
        case P_SEED: seed = value; return true;
        // The walk. None of them re-derives anything: the weights are
        // computed on the next advance, from whatever these say by then, so a
        // knob sweep costs nothing until a chord is due.
        case P_FIFTHS:  if (value > 100) return false; fifths = value ? value : DEFAULT_FIFTHS; return true;
        case P_SMOOTH:  if (value > 100) return false; smooth = value; return true;
        case P_LEADING: if (value > 100) return false; leading = value ? value : DEFAULT_LEADING; return true;
        case P_SPREAD:  if (value > 100) return false; spread = value ? value : DEFAULT_SPREAD; return true;
        case P_DRIFT:   if (value > 100) return false; drift = value; return true;
        case P_KEY:     if (value >= global_scale::KEY_MODES) return false; key = value; return true;
        default: return false;
    }
}

uint8_t Harmony::get_param(uint16_t index) const {
    switch (index){
        case P_PHRASE: return phrase;
        case P_CADENCE: return cadence;
        case P_GRAVITY: return gravity;
        case P_LOOP: return loop;
        case P_ROOT: return root;
        case P_SCALE: return scale;
        case P_VELOCITY: return velocity;
        case P_CHANNEL: return channel;
        case P_SEED: return seed;
        case P_FIFTHS:  return fifths;
        case P_SMOOTH:  return smooth;
        case P_LEADING: return leading;
        case P_SPREAD:  return spread;
        case P_DRIFT:   return drift;
        case P_KEY:     return key;
        default: return 0;
    }
}
