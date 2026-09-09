#include "algorithm/midi/chord.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_scale.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[9] = {
    {"voices",     0, Chord::MAX_INTERVALS, 0, PARAM_NUMBER, nullptr},
    {"interval 1", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 2", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 3", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 4", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 5", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 6", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"scale",      0, SCALE_COUNT - 1, 0, PARAM_ENUM,        PARAM_SCALE_NAMES},
    {"root",       0, 11,              0, PARAM_PITCH_CLASS, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 9, PARAMS}};

static const char* const IN_NAMES[2] = {"note in", "root"};
static const char* const OUT_NAMES[1] = {"chord out"};

const AlgorithmDescriptor Chord::descriptor = {
    ALGO_CHORD, "Chord", 2, 1, 1, 9, IN, OUT, sizeof(Chord), false, construct_node<Chord>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "One note in, a chord out: the root plus up to six intervals of a scale, held together." };

// The voicing can move under a held chord: every voice already in the air is
// released from the ledger at the pitch it was emitted at, so re-voicing -
// or a change of scale, here or on the module - changes what the *next*
// note-on plays and strands nothing.
bool Chord::set_param(uint16_t index, uint8_t value){
    if (index == 0){
        if (value > MAX_INTERVALS) return false;
        n_intervals = value;
        return true;
    }
    if (index >= 1 && index <= MAX_INTERVALS){
        intervals[index - 1] = (int8_t)value;
        return true;
    }
    if (index == P_SCALE){
        if (value >= SCALE_COUNT) return false;
        scale = value;
        return true;
    }
    if (index == P_ROOT){
        root = (uint8_t)(value % 12u);
        return true;
    }
    return false;
}

uint8_t Chord::get_param(uint16_t index) const {
    if (index == 0) return n_intervals;
    if (index >= 1 && index <= MAX_INTERVALS) return (uint8_t)intervals[index - 1];
    if (index == P_SCALE) return scale;
    if (index == P_ROOT) return root;
    return 0;
}

Chord::Chord(const NodeConfig& config) :
    in(config.in_bus[0]),
    root_in(config.in_bus[1]),
    out(config.out_bus[0]),
    n_intervals(config.params[0] > MAX_INTERVALS ? MAX_INTERVALS : config.params[0]),
    scale(config.params[P_SCALE]),
    root((uint8_t)(config.params[P_ROOT] % 12u)),
    intervals(), sounding()
{
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) intervals[i] = (int8_t)config.params[i + 1];
}

uint16_t Chord::active_mask() const {
    return global_scale::resolve_id(scale);
}

uint8_t Chord::active_root() const {
    if (root_in != NO_BUS) return root;
    return global_scale::resolve_root(scale, root);
}

void Chord::process(BusManager& bus, uint32_t){
    // The root first, so a root and a note arriving in the same pass agree,
    // as in NoteQuantise.
    if (root_in != NO_BUS){
        const uint8_t rn = bus.note_count(root_in);
        for (uint8_t i = 0; i < rn; i++){
            const MidiEvent e = bus.note_read(root_in, i);
            if (is_note_on(e)) root = (uint8_t)(e.data1 % 12u);
        }
    }

    const uint16_t mask = active_mask();
    const uint8_t key = active_root();
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);
            continue;
        }
        if (!is_note_on(e)){
            bus.note_write(out, e);
            continue;
        }
        // The root voice, in key. In the chromatic scale this is the note
        // itself and every interval below is a semitone, which is what this
        // algorithm did before it had a scale.
        const uint8_t base = scale_quantise(e.data1, key, mask);
        const int16_t degree = semitone_to_scale_degree((int16_t)((int16_t)base - (int16_t)key), mask);
        sounding.emit(bus, out, e.data1, base, e.data2, e.channel);
        for (uint8_t v = 0; v < n_intervals; v++){
            const int16_t note = (int16_t)key
                               + scale_degree_to_semitone((int16_t)(degree + intervals[v]), mask);
            if (note < 0 || note > 127) continue;
            if (note == base) continue;                                   // no unisons
            sounding.emit(bus, out, e.data1, (uint8_t)note, e.data2, e.channel);
        }
    }
}

void Chord::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
