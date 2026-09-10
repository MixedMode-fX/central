#include "algorithm/midi/harmony.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Note, Domain::CV};

// What a chord wants to do next.
//
// One row per degree of the key (I ii iii IV V vi vii), one column per degree
// it might move to, the weight being how much that style wants it. The
// numbers are relative within a row; nothing normalises them, because a
// weighted draw does not need it and a row that sums to a round number would
// be a coincidence rather than a fact about music.
//
// This is a first-order model - where the walk goes next depends on where it
// is and nothing else - which is what the harmonic-generation literature uses
// and what fits in a node. It is not a claim that music is first order. It is
// a claim that a first-order table with a phrase structure over it sounds far
// more like music than a uniform draw, and `walk` is in the list so that can
// be heard rather than argued.
static const uint8_t STYLE_TABLE[Harmony::HARM_STYLES][Harmony::DEGREES][Harmony::DEGREES] = {
    // pop: the dominant pulls hard, IV and vi are everywhere, V-vi is the
    // deceptive cadence that keeps a phrase from ending early.
    {
        //  I  ii iii IV   V  vi vii
        {   1,  4,  2,  8,  8,  6,  1 },   // I
        {   2,  1,  1,  3, 10,  2,  1 },   // ii
        {   1,  2,  1,  7,  2,  5,  1 },   // iii
        {   8,  2,  1,  1,  8,  4,  1 },   // IV
        {  10,  1,  1,  4,  1,  7,  1 },   // V
        {   3,  4,  1,  8,  5,  1,  1 },   // vi
        {   9,  1,  3,  1,  2,  1,  1 },   // vii
    },
    // modal: plagal motion and the degree below the tonic. The dominant is
    // just another chord, which is what stops a drone sounding like a cadence
    // waiting to happen.
    {
        {   3,  3,  2,  8,  3,  5,  6 },
        {   6,  1,  2,  4,  2,  3,  3 },
        {   6,  2,  1,  4,  2,  4,  3 },
        {   9,  2,  2,  1,  2,  3,  4 },
        {   8,  2,  2,  4,  1,  3,  3 },
        {   7,  2,  2,  5,  2,  1,  4 },
        {   9,  2,  2,  4,  2,  3,  1 },
    },
    // jazz: down a fifth, which in degrees is three steps up, over and over.
    // ii-V-I falls out of the table rather than being written into it.
    {
        {   1,  6,  1, 14,  1,  2,  1 },   // I  -> IV, or ii to start a ii-V
        {   1,  1,  1,  1, 16,  1,  1 },   // ii -> V
        {   1,  2,  1,  1,  1, 14,  1 },   // iii-> vi
        {   2,  1,  1,  1,  4,  1, 14 },   // IV -> vii, or V
        {  16,  1,  1,  1,  1,  3,  1 },   // V  -> I, or vi for a deception
        {   1, 16,  1,  2,  1,  1,  1 },   // vi -> ii
        {   3,  1, 14,  1,  1,  1,  1 },   // vii-> iii
    },
    // walk: every degree equally. The null model - what the others are
    // measured against, and what a patch uses when it wants no opinion.
    {
        {   1,  1,  1,  1,  1,  1,  1 },
        {   1,  1,  1,  1,  1,  1,  1 },
        {   1,  1,  1,  1,  1,  1,  1 },
        {   1,  1,  1,  1,  1,  1,  1 },
        {   1,  1,  1,  1,  1,  1,  1 },
        {   1,  1,  1,  1,  1,  1,  1 },
        {   1,  1,  1,  1,  1,  1,  1 },
    },
    // pedal: almost always home. For a patch that should breathe rather than
    // move - and the place `gravity` reaches from any of the others.
    {
        {  12,  1,  1,  3,  3,  2,  1 },
        {  12,  2,  1,  1,  1,  1,  1 },
        {  12,  1,  2,  1,  1,  1,  1 },
        {  12,  1,  1,  2,  1,  1,  1 },
        {  12,  1,  1,  1,  2,  1,  1 },
        {  12,  1,  1,  1,  1,  2,  1 },
        {  12,  1,  1,  1,  1,  1,  2 },
    },
};

static const char* const STYLE_NAMES[Harmony::HARM_STYLES] = {"pop", "modal", "jazz", "walk", "pedal"};

static const ParamDescriptor PARAMS[10] = {
    {"style",    Harmony::HARM_POP, Harmony::HARM_STYLES, Harmony::HARM_POP, PARAM_ENUM, STYLE_NAMES},
    {"phrase",   2, Harmony::MAX_PHRASE, 4, PARAM_NUMBER,  nullptr},
    {"cadence",  0, 100,               75, PARAM_PERCENT, nullptr},
    {"gravity",  0, 100,                0, PARAM_PERCENT, nullptr},
    {"loop",     0, 1,                  0, PARAM_BOOL,    nullptr},
    {"root",     0, 127,               48, PARAM_PITCH,   nullptr},
    {"scale",    0, SCALE_COUNT - 1,    0, PARAM_ENUM,    PARAM_SCALE_NAMES},
    {"velocity", 1, 127,              100, PARAM_NUMBER,  nullptr},
    {"channel",  1, 16,                 1, PARAM_CHANNEL, nullptr},
    {"seed",     0, 255,                0, PARAM_NUMBER,  nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 10, PARAMS}};

static const char* const IN_NAMES[2] = {"advance", "reset"};
static const char* const OUT_NAMES[2] = {"root", "degree"};

const AlgorithmDescriptor Harmony::descriptor = {
    ALGO_HARMONY, "Harmony", 2, 1, 2, 10, IN, OUT, sizeof(Harmony), false,
    construct_node<Harmony>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A chord progression in the module's key: walks the degrees, plays the root.",
    CATEGORY_MIDI };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Harmony::Harmony(const NodeConfig& config) :
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    note_out(config.out_bus[0]),
    cv_out(config.out_bus[1]),
    style(clamp_enum(config.params[0], HARM_STYLES, HARM_POP)),
    phrase(config.params[1] ? (config.params[1] > MAX_PHRASE ? MAX_PHRASE : config.params[1]) : (uint8_t)4),
    cadence(config.params[2] ? (config.params[2] > 100 ? (uint8_t)100 : config.params[2]) : (uint8_t)75),
    gravity(config.params[3] > 100 ? (uint8_t)100 : config.params[3]),
    loop(config.params[4] ? 1 : 0),
    root(config.params[5] ? (uint8_t)(config.params[5] & 0x7F) : (uint8_t)48),
    scale(config.params[6] < SCALE_COUNT ? config.params[6] : (uint8_t)0),
    velocity(config.params[7] ? (uint8_t)(config.params[7] & 0x7F) : (uint8_t)100),
    channel(config.params[8] ? config.params[8] : (uint8_t)1),
    seed(config.params[9]),
    current(0), position(0), recorded(0), started(false), at_first(true), written(),
    rng(config.params[9] ? (uint32_t)(config.params[9] * 2654435761u) : entropy::seed()),
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
    int16_t pitch = (int16_t)global_scale::resolve_tonic(scale, root)
                  + scale_degree_to_semitone((int16_t)deg, mask);
    while (pitch > 127) pitch -= 12;
    while (pitch < 0) pitch += 12;
    return (uint8_t)pitch;
}

uint8_t Harmony::choose(){
    const uint8_t n = usable_degrees();
    if (n <= 1) return 0;

    // The cadence: the phrase's last chord resolves. This is the one thing
    // that turns a walk into a period, and it is checked before the table so
    // that no style can talk it out of a resolution.
    if (position + 1u == phrase && rng.chance(cadence)) return 0;

    const uint8_t* row = STYLE_TABLE[style - 1][current < DEGREES ? current : 0];
    uint32_t weight[DEGREES];
    uint32_t total = 0;
    for (uint8_t j = 0; j < n; j++){
        // `gravity` mixes the style with a weight on the tonic. The table's
        // own weights are multiplied out rather than divided down, because
        // dividing would round a weight of 1 to nothing long before gravity
        // reached the top and the walk would lose its rarest moves first -
        // which is the opposite of a gravity control, whose job is to shorten
        // every move evenly.
        uint32_t w = (uint32_t)row[j] * (uint32_t)(100u - gravity);
        if (j == 0) w += (uint32_t)gravity * TONIC_PULL;
        weight[j] = w;
        total += w;
    }
    if (total == 0) return 0;

    // Xorshift32::below is bias-free and takes a byte; these weights run into
    // the thousands, where the modulo bias its comment warns about is one part
    // in four hundred thousand of one draw. That is smaller than the
    // difference between any two weights in the tables.
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
    } else if (started){
        // Re-voiced when what it should be playing changes - the key moving
        // under it, or the root or the scale being edited. Chord does exactly
        // this and for the same reason: a held note that cannot follow the
        // key drags the whole patch out of it, and the release still comes
        // from the ledger, so re-voicing cannot strand anything.
        const uint8_t want = pitch_of(current);
        if (sounding.count() == 0 || sounding.at(0).note != want) strike(bus, current);
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
        case 0: if (value == 0 || value > HARM_STYLES) return false; style = value; return true;
        case 1:
            if (value < 2 || value > MAX_PHRASE) return false;
            if (value == phrase) return true;
            // A phrase of a different length is a different phrase, so the
            // one that was recorded is no longer the piece.
            phrase = value;
            recorded = 0;
            if (position >= phrase) position = 0;
            return true;
        case 2: if (value > 100) return false; cadence = value; return true;
        case 3: if (value > 100) return false; gravity = value; return true;
        case 4:
            if (value > 1) return false;
            if (value == loop) return true;
            loop = value;
            recorded = 0;          // on: record the next phrase. off: walk again.
            return true;
        case 5: if (value > 127) return false; root = value; return true;
        case 6: if (value >= SCALE_COUNT) return false; scale = value; return true;
        case 7: if (value == 0 || value > 127) return false; velocity = value; return true;
        case 8: if (value == 0 || value > 16) return false; channel = value; return true;
        case 9: seed = value; return true;
        default: return false;
    }
}

uint8_t Harmony::get_param(uint16_t index) const {
    switch (index){
        case 0: return style;
        case 1: return phrase;
        case 2: return cadence;
        case 3: return gravity;
        case 4: return loop;
        case 5: return root;
        case 6: return scale;
        case 7: return velocity;
        case 8: return channel;
        case 9: return seed;
        default: return 0;
    }
}
