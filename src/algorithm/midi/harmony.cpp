#include "algorithm/midi/harmony.h"
#include "node/registry.h"
#include "midi/global_key.h"
#include "hal/midi_types.h"
#include "midi/root_motion.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Note, Domain::CV};

static const ParamDescriptor PARAMS[Harmony::N_PARAMS] = {
    {"phrase",   2, Harmony::MAX_PHRASE, 4, PARAM_NUMBER,  nullptr},
    {"cadence",  0, 100,              75, PARAM_PERCENT, nullptr},
    {"gravity",  0, 100,               0, PARAM_PERCENT, nullptr},
    {"loop",     0, Harmony::MAX_PHRASE, 0, PARAM_NUMBER, nullptr},
    {"octave",   0, KEY_MAX_OCTAVE, 0, PARAM_ENUM, PARAM_OCTAVE_NAMES},
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
};
// Three mechanisms, and the storage order interleaves them: the parameter
// order is the preset format and cannot be rearranged, so the groups say what
// belongs together and the editor puts the runs back in one place. Groups
// carrying the same label are one section; the array's order is the order
// they are shown in, which is why the walk leads.
static const char* const WALK = "the walk";
static const char* const FORM = "the form";
static const char* const OUTPUT = "the output";
static const ParamGroup GROUPS[7] = {
    {Harmony::P_FIFTHS,  1, 4, &PARAMS[Harmony::P_FIFTHS],  WALK},   // fifths..spread
    {Harmony::P_GRAVITY, 1, 1, &PARAMS[Harmony::P_GRAVITY], WALK},
    {Harmony::P_SEED,    1, 1, &PARAMS[Harmony::P_SEED],    WALK},
    {Harmony::P_PHRASE,  1, 2, &PARAMS[Harmony::P_PHRASE],  FORM},   // phrase, cadence
    {Harmony::P_LOOP,    1, 1, &PARAMS[Harmony::P_LOOP],    FORM},
    {Harmony::P_DRIFT,   1, 1, &PARAMS[Harmony::P_DRIFT],   FORM},
    {Harmony::P_OCTAVE,  1, 3, &PARAMS[Harmony::P_OCTAVE],  OUTPUT}, // octave, velocity, channel
};

static const char* const IN_NAMES[2] = {"advance", "reset"};
static const char* const OUT_NAMES[2] = {"root", "degree"};

const AlgorithmDescriptor Harmony::descriptor = {
    ALGO_HARMONY, "Harmony", 2, 1, 2, Harmony::N_PARAMS, IN, OUT, sizeof(Harmony), false,
    construct_node<Harmony>, GROUPS, 7, IN_NAMES, OUT_NAMES,
    "A chord progression in the module's key: weighs every move against the scale, plays the root.",
    CATEGORY_MIDI,
    true };   // reads_key: every pitch it plays comes from the key


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
    loop(config.params[P_LOOP] > MAX_PHRASE ? MAX_PHRASE : config.params[P_LOOP]),
    octave(config.params[P_OCTAVE] <= KEY_MAX_OCTAVE ? config.params[P_OCTAVE] : (uint8_t)0),
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
    current(0), position(0), loop_pos(0), recorded(0), started(false), at_first(true), written(),
    rng(config.params[P_SEED] ? (uint32_t)(config.params[P_SEED] * 2654435761u) : entropy::seed()),
    sounding()
{
    if (phrase < 2) phrase = 2;
}

uint8_t Harmony::usable_degrees() const {
    const uint8_t n = scale_size(global_key::mask());
    return n < DEGREES ? n : DEGREES;
}

// Roots fold rather than disappear. A degree that would leave 0..127 is the
// same pitch class an octave the other way, and for a *root* that is the
// right answer - unlike a melody note, where Transpose and CvToNote drop it,
// because there a folded note is a wrong note and a missing one is a rest.
uint8_t Harmony::pitch_of(uint8_t deg) const {
    const uint16_t mask = global_key::mask();
    const uint8_t n = usable_degrees();
    if (deg >= n && n) deg = (uint8_t)(n - 1u);
    int16_t pitch = (int16_t)global_key::tonic(octave)
                  + scale_degree_to_semitone((int16_t)deg, mask);
    while (pitch > 127) pitch -= 12;
    while (pitch < 0) pitch += 12;
    return (uint8_t)pitch;
}

// Real pitch classes, not scale intervals: root_motion works in semitones
// above the tonic, because that is all a weight needs, and anything asking
// what the chord *is* - a display, a test - wants the notes it will hear. So
// the set is turned to where the key actually sits.
uint16_t Harmony::triad_of(uint8_t deg) const {
    const uint8_t n = usable_degrees();
    if (!n) return 0;
    if (deg >= n) deg = (uint8_t)(n - 1u);
    const uint16_t set = root_motion::triad(root_motion::degrees_of(global_key::mask(), n), deg);
    const uint8_t tonic = global_key::root();
    return (uint16_t)(((set << tonic) | (set >> (12u - tonic))) & 0x0FFFu);
}

// Every move weighed against the scale, then shaped. Split out of choose()
// because it is also what proves the walk: `likeliest_from` reads the same
// numbers with none of the randomness on top.
uint8_t Harmony::weigh(uint8_t from, uint32_t* weight) const {
    const uint16_t mask = global_key::mask();
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

uint8_t Harmony::choose(uint8_t from, uint8_t at){
    const uint8_t n = usable_degrees();
    if (n <= 1) return 0;

    // The cadence: the phrase's last chord resolves. This is the one thing
    // that turns a walk into a period, and it is checked before the weights
    // so that nothing about the walk can talk it out of a resolution.
    if (at + 1u == phrase && rng.chance(cadence)) return 0;

    uint32_t weight[DEGREES];
    weigh(from, weight);
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

// A written loop is the piece, so a control that shapes the walk is a rewrite
// of it - not a request to hear the old one again with new settings that
// reach nothing. Before this, every control over the walk went quiet the
// moment a loop was written out, and the only way to hear one again was to
// set `loop` to zero and back: a length used as a button, which is the
// clearest sign a control was missing.
//
// The new piece is walked from the chord that is sounding, so it follows on
// from the music rather than cutting to something unrelated, and from the top
// of a phrase, so its cadence lands on its last chord rather than wherever
// the old one happened to be. Playback restarts at slot 0 for the same
// reason: what has just been written is a piece, and a piece is heard from
// the beginning.
//
// It is written in full rather than re-captured one chord per advance,
// because a chord here is a bar: re-capturing a loop of four would take four
// bars to say what a knob said instantly, which is the complaint this
// answers.
void Harmony::recompose(){
    if (!loop) return;
    uint8_t from = started ? current : 0;
    uint8_t at = 0;
    for (uint8_t i = 0; i < loop; i++){
        from = choose(from, at);
        written[i] = from;
        at = (uint8_t)((at + 1u) % phrase);
    }
    recorded = loop;
    loop_pos = 0;
    position = 0;
}

void Harmony::restart(){
    // What reset means in every sequencer here: the next advance plays the
    // first step. It does not silence what is sounding - a root that dropped
    // out between the reset and the next chord would be a hole in the music,
    // and the next advance replaces it anyway.
    //
    // The loop goes back to its first chord too, and a *written* one is kept:
    // it is the piece, and a reset asks to hear it from the top rather than
    // to write another one. A half-written one is not a piece yet, so it is
    // caught again from here - which is also the only way its first chord can
    // be the tonic the reset is about to play.
    position = 0;
    loop_pos = 0;
    if (recorded < loop) recorded = 0;
    at_first = true;
}

void Harmony::process(BusManager& bus, uint32_t){
    if (reset_in.rising(bus)) restart();

    if (advance_in.rising(bus)){
        const bool written_out = loop && recorded >= loop;   // the loop is the piece now
        uint8_t deg;
        if (written_out){
            deg = written[loop_pos];               // the loop, as written
            // An accident that happens once is a glitch and one that comes
            // back is a decision, so a redraw *replaces* the chord it landed
            // on. A loop with a few percent of drift is a piece that is
            // recognisably itself and never quite the same twice.
            if (drift && rng.chance(drift)){
                deg = choose(current, position);
                written[loop_pos] = deg;
            }
        } else if (at_first){
            deg = 0;                               // the first advance is the tonic
        } else {
            deg = choose(current, position);
        }
        at_first = false;

        // A loop is captured from the top of a phrase, so setting a length
        // halfway through one waits for the next rather than catching a
        // phrase that begins in the middle. After that the loop fills
        // straight through, however long the phrase is: its length is its
        // own, not the cadence's.
        if (loop && recorded < loop && (recorded || position == 0)){
            written[recorded] = deg;
            recorded = (uint8_t)(recorded + 1u);
        }

        strike(bus, deg);
        position = (uint8_t)((position + 1u) % phrase);
        // Only the playback counter moves: while the loop is being captured
        // the slot to write is `recorded`, and it lands on 0 when the last
        // one is written, which is where playback starts.
        if (written_out) loop_pos = (uint8_t)((loop_pos + 1u) % loop);
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
            // The phrase is how often the music resolves and the loop is how
            // much of it repeats: two different lengths, and moving one does
            // not resize the other. It does rewrite it, because where the
            // cadences fall inside a loop is part of what the loop *is*.
            phrase = value;
            if (position >= phrase) position = 0;
            recompose();
            return true;
        case P_CADENCE:
            if (value > 100) return false;
            if (value == cadence) return true;
            cadence = value;
            recompose();
            return true;
        case P_GRAVITY:
            if (value > 100) return false;
            if (value == gravity) return true;
            gravity = value;
            recompose();
            return true;
        case P_LOOP:
            if (value > MAX_PHRASE) return false;
            if (value == loop) return true;
            // A loop of a different length is a different piece. Setting one
            // does not write it: what a loop holds is what the walk played,
            // so it is captured from the next top of a phrase, one chord per
            // advance, and the empty slots filling up are the module saying
            // so. `recompose` is for the controls that rewrite a piece that
            // already exists. 0 walks on.
            loop = value;
            recorded = 0;
            loop_pos = 0;
            return true;
        case P_OCTAVE: if (value > KEY_MAX_OCTAVE) return false; octave = value; return true;
        case P_VELOCITY: if (value == 0 || value > 127) return false; velocity = value; return true;
        case P_CHANNEL: if (value == 0 || value > 16) return false; channel = value; return true;
        case P_SEED:
            if (value == seed) return true;
            seed = value;
            // The seed has to reach the generator. Storing the byte and
            // seeding only in the constructor made this the one control that
            // did nothing whatever until the patch was loaded again - and the
            // one control whose whole purpose is to make a walk repeatable.
            // 0 is "draw from the pool", the same as it means at construction.
            rng.reseed(value ? (uint32_t)(value * 2654435761u) : entropy::seed());
            recompose();
            return true;
        // The walk. Each rewrites a running loop and nothing else: the
        // weights themselves are computed on the next advance, from whatever
        // these say by then, so a knob sweep on a node with no loop costs
        // nothing until a chord is due.
        case P_FIFTHS: {
            if (value > 100) return false;
            // Compared against the value it will *become*, not the byte
            // that arrived: zero is the descriptor's default here as
            // everywhere, so writing zero over a parameter already sitting at
            // its default is not a change and must not rewrite the loop.
            const uint8_t want = value ? value : DEFAULT_FIFTHS;
            if (want == fifths) return true;
            fifths = want;
            recompose();
            return true;
        }
        case P_SMOOTH:
            if (value > 100) return false;
            if (value == smooth) return true;
            smooth = value;
            recompose();
            return true;
        case P_LEADING: {
            if (value > 100) return false;
            const uint8_t want = value ? value : DEFAULT_LEADING;
            if (want == leading) return true;
            leading = want;
            recompose();
            return true;
        }
        case P_SPREAD: {
            if (value > 100) return false;
            const uint8_t want = value ? value : DEFAULT_SPREAD;
            if (want == spread) return true;
            spread = want;
            recompose();
            return true;
        }
        // Not the walk: drift says how often a loop is redrawn, not what the
        // redraw produces, so moving it is not a rewrite of the piece.
        case P_DRIFT:   if (value > 100) return false; drift = value; return true;
        default: return false;
    }
}

uint8_t Harmony::get_param(uint16_t index) const {
    switch (index){
        case P_PHRASE: return phrase;
        case P_CADENCE: return cadence;
        case P_GRAVITY: return gravity;
        case P_LOOP: return loop;
        case P_OCTAVE: return octave;
        case P_VELOCITY: return velocity;
        case P_CHANNEL: return channel;
        case P_SEED: return seed;
        case P_FIFTHS:  return fifths;
        case P_SMOOTH:  return smooth;
        case P_LEADING: return leading;
        case P_SPREAD:  return spread;
        case P_DRIFT:   return drift;
        default: return 0;
    }
}
