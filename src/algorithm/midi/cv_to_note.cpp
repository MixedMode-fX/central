#include "algorithm/midi/cv_to_note.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "hal/midi_types.h"

static const Domain IN[3] = {Domain::CV, Domain::Gate, Domain::CV};
static const Domain OUT[1] = {Domain::Note};

static const char* const MAP_NAMES[CvToNote::CVN_MAPS] = {"degree", "snap"};
static const char* const MODE_NAMES[CvToNote::CVN_MODES] = {"auto", "track", "trigger"};
static const char* const POLARITY_NAMES[CvToNote::CVN_POLARITIES] = {"bipolar", "unipolar"};

static const ParamDescriptor PARAMS[9] = {
    {"map",      CvToNote::CVN_DEGREE,  CvToNote::CVN_MAPS,       CvToNote::CVN_DEGREE,  PARAM_ENUM,   MAP_NAMES},
    {"root",     0, 127,                48, PARAM_PITCH,   nullptr},
    {"range",    1, CvToNote::MAX_RANGE, 2, PARAM_NUMBER,  nullptr},
    {"scale",    0, SCALE_COUNT - 1,     0, PARAM_ENUM,    PARAM_SCALE_NAMES},
    {"mode",     CvToNote::CVN_AUTO,    CvToNote::CVN_MODES,      CvToNote::CVN_AUTO,    PARAM_ENUM,   MODE_NAMES},
    {"polarity", CvToNote::CVN_BIPOLAR, CvToNote::CVN_POLARITIES, CvToNote::CVN_BIPOLAR, PARAM_ENUM,   POLARITY_NAMES},
    {"gate",     0, 255,                 0, PARAM_MILLIS,  nullptr},
    {"velocity", 1, 127,               100, PARAM_NUMBER,  nullptr},
    {"channel",  1, 16,                  1, PARAM_CHANNEL, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 9, PARAMS}};

static const char* const IN_NAMES[3] = {"cv", "trigger", "velocity"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor CvToNote::descriptor = {
    ALGO_CV_TO_NOTE, "CvToNote", 3, 1, 1, 9, IN, OUT, sizeof(CvToNote), false,
    construct_node<CvToNote>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "The quantiser: a control signal becomes a melody, in the module's key.",
    CATEGORY_MIDI };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

CvToNote::CvToNote(const NodeConfig& config) :
    in(config.in_bus[0]),
    trigger_in(config.in_bus[1]),
    velocity_in(config.in_bus[2]),
    out(config.out_bus[0]),
    map(clamp_enum(config.params[0], CVN_MAPS, CVN_DEGREE)),
    root(config.params[1] ? (uint8_t)(config.params[1] & 0x7F) : (uint8_t)48),
    range(config.params[2] ? (config.params[2] > MAX_RANGE ? MAX_RANGE : config.params[2]) : (uint8_t)2),
    scale(config.params[3] < SCALE_COUNT ? config.params[3] : (uint8_t)0),
    mode(clamp_enum(config.params[4], CVN_MODES, CVN_AUTO)),
    polarity(clamp_enum(config.params[5], CVN_POLARITIES, CVN_BIPOLAR)),
    gate_ms(config.params[6]),
    velocity(config.params[7] ? (uint8_t)(config.params[7] & 0x7F) : (uint8_t)100),
    channel(config.params[8] ? config.params[8] : (uint8_t)1),
    trigger(config.in_bus[1]),
    last_pitch(0xFF),
    due_us(0), timed(false),
    sounding()
{}

uint16_t CvToNote::active_mask() const {
    return global_scale::resolve_id(scale);
}

// The octave is this node's, the pitch class is the key's. A pitch class
// cannot name an octave and a key cannot have one, so the two halves come
// from the two places that can say them - and a node that names its own scale
// keeps its own root entirely, which is the rule everywhere else.
uint8_t CvToNote::active_root() const {
    const uint8_t pc = global_scale::resolve_root(scale, (uint8_t)(root % 12u));
    const int16_t base = (int16_t)root - (int16_t)(root % 12u);
    int16_t tonic = (int16_t)(base + pc);
    if (tonic > 127) tonic -= 12;
    return (uint8_t)tonic;
}

// The matrix's reading of a signal (control/mod_matrix.cpp), so "bipolar"
// means the same thing to a route and to this node.
int32_t CvToNote::level_of(int16_t cv) const {
    int32_t level = (polarity == CVN_BIPOLAR) ? ((int32_t)cv + CV_HALF)
                                              : cv_clamp_unipolar((int32_t)cv);
    if (level < 0) level = 0;
    if (level > CV_MAX) level = CV_MAX;
    return level;
}

uint8_t CvToNote::pitch_for(int16_t cv) const {
    const uint16_t mask = active_mask();
    const uint8_t tonic = active_root();
    const int32_t level = level_of(cv);
    int32_t pitch;

    if (map == CVN_DEGREE){
        // The range is the scale's notes, evenly: every degree is as likely
        // as every other, and there is no pitch to snap because every value
        // of the signal already names one.
        const int32_t degrees = (int32_t)scale_size(mask) * (int32_t)range;
        int32_t degree = (level * degrees) / CV_FULL;
        if (degree >= degrees) degree = degrees - 1;      // the very top
        pitch = (int32_t)tonic + scale_degree_to_semitone((int16_t)degree, mask);
    } else {
        const int32_t semitones = 12 * (int32_t)range;
        int32_t offset = (level * semitones) / CV_FULL;
        if (offset >= semitones) offset = semitones - 1;
        pitch = (int32_t)tonic + offset;
        if (pitch < 0 || pitch > 127) return 0xFF;
        pitch = scale_quantise((uint8_t)pitch, (uint8_t)(tonic % 12u), mask);
    }
    // A pitch off the end of MIDI is dropped rather than folded, which is
    // what Transpose and Chord do with the same problem: a folded note is a
    // wrong note, and a missing one is a rest.
    if (pitch < 0 || pitch > 127) return 0xFF;
    return (uint8_t)pitch;
}

void CvToNote::strike(BusManager& bus, uint32_t now_us, uint8_t pitch){
    if (pitch == 0xFF){
        // Nothing to play. Whatever was sounding still gets its note-off:
        // a level that walks off the end of the keyboard is a rest, not a
        // held note.
        sounding.release_all(bus, out);
        last_pitch = 0xFF;
        timed = false;
        return;
    }

    uint8_t vel = velocity;
    if (velocity_in != NO_BUS){
        const int32_t level = cv_clamp_unipolar((int32_t)bus.cv_read(velocity_in));
        vel = (uint8_t)(1 + (level * 126) / CV_MAX);
    }

    sounding.release_all(bus, out);
    // The ledger keys on the source note; this node has no incoming note, and
    // one voice, so every emission shares one key and release_all is the only
    // release it needs.
    sounding.emit(bus, out, 0, pitch, vel, channel);
    last_pitch = pitch;
    timed = gate_ms != 0;
    if (timed) due_us = now_us + (uint32_t)gate_ms * 1000u;
}

void CvToNote::process(BusManager& bus, uint32_t now_us){
    const bool edge = trigger.rising(bus);
    const bool triggered = (mode == CVN_TRIGGER)
                        || (mode == CVN_AUTO && trigger_in != NO_BUS);

    if (triggered){
        // A trigger retriggers, whether or not the pitch moved: that is what
        // an edge means, and it is what makes a SampleHold plus a clock into
        // a sequence rather than a held note.
        if (edge) strike(bus, now_us, pitch_for(bus.cv_read(in)));
    } else {
        // Tracking: the level is read every pass and a note is struck only
        // when the *pitch* changes, so a signal sweeping through one tone of
        // the scale is one note and not a hundred. The comparison is against
        // what was last struck rather than against what is still sounding -
        // otherwise a `gate` shorter than the sweep would restrike the same
        // note the pass after its note-off, for ever.
        const uint8_t pitch = pitch_for(bus.cv_read(in));
        if (pitch != last_pitch) strike(bus, now_us, pitch);
    }

    // A timed note is released on time whichever mode struck it. The
    // subtraction is unsigned, so a wrap of now_us is not a 71-minute note.
    if (timed && (uint32_t)(now_us - due_us) < 0x80000000u){
        sounding.release_all(bus, out);
        timed = false;
    }
}

void CvToNote::silence(BusManager& bus){
    sounding.release_all(bus, out);
    last_pitch = 0xFF;
    timed = false;
}

bool CvToNote::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: if (value == 0 || value > CVN_MAPS) return false; map = value; return true;
        case 1: if (value > 127) return false; root = value; return true;
        case 2: if (value == 0 || value > MAX_RANGE) return false; range = value; return true;
        case 3: if (value >= SCALE_COUNT) return false; scale = value; return true;
        case 4: if (value == 0 || value > CVN_MODES) return false; mode = value; return true;
        case 5: if (value == 0 || value > CVN_POLARITIES) return false; polarity = value; return true;
        case 6: gate_ms = value; return true;
        case 7: if (value == 0 || value > 127) return false; velocity = value; return true;
        case 8: if (value == 0 || value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t CvToNote::get_param(uint16_t index) const {
    switch (index){
        case 0: return map;
        case 1: return root;
        case 2: return range;
        case 3: return scale;
        case 4: return mode;
        case 5: return polarity;
        case 6: return gate_ms;
        case 7: return velocity;
        case 8: return channel;
        default: return 0;
    }
}
