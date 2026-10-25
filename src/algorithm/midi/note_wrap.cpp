#include "algorithm/midi/note_wrap.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_key.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[NoteWrap::N_PARAMS] = {
    {"octave",  0, KEY_MAX_OCTAVE, 0, PARAM_ENUM, PARAM_OCTAVE_NAMES},
    {"low",     PARAM_CENTRE - NoteWrap::MAX_LOW, PARAM_CENTRE + NoteWrap::MAX_LOW,
                PARAM_CENTRE, PARAM_CENTRED, nullptr},
    {"range",    1, NoteWrap::MAX_RANGE, NoteWrap::DEFAULT_RANGE, PARAM_NUMBER, nullptr},
    {"channel", 0, 16, 0, PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, NoteWrap::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"notes in", "root"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor NoteWrap::descriptor = {
    ALGO_NOTE_WRAP, "NoteWrap", 2, 1, 1, NoteWrap::N_PARAMS, IN, OUT, sizeof(NoteWrap), false,
    construct_node<NoteWrap>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Folds notes by octaves into a range set from the root, the key's or a played one.",
    CATEGORY_MIDI,
    true };   // reads_key: the root is the key's tonic unless one is played

static int8_t clamp_low(uint8_t stored){
    const int8_t value = param_centred(stored);
    if (value > NoteWrap::MAX_LOW) return NoteWrap::MAX_LOW;
    if (value < -NoteWrap::MAX_LOW) return (int8_t)-NoteWrap::MAX_LOW;
    return value;
}

static uint8_t clamp_range(uint8_t stored){
    if (stored == 0) return NoteWrap::DEFAULT_RANGE;
    return stored > NoteWrap::MAX_RANGE ? (uint8_t)NoteWrap::MAX_RANGE : stored;
}

NoteWrap::NoteWrap(const NodeConfig& config) :
    in(config.in_buses[0]),
    root_in(config.in_buses[1]),
    out(config.out_buses[0]),
    octave(config.params[P_OCTAVE] > KEY_MAX_OCTAVE ? (uint8_t)0 : config.params[P_OCTAVE]),
    low(clamp_low(config.params[P_LOW])),
    range(clamp_range(config.params[P_RANGE])),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    played_root(NO_ROOT),
    sounding()
{}

uint8_t NoteWrap::root() const {
    return played_root != NO_ROOT ? global_key::placed(played_root, octave)
                                  : global_key::tonic(octave);
}

static uint8_t on_keyboard(int16_t pitch){
    return (uint8_t)(pitch < 0 ? 0 : pitch > 127 ? 127 : pitch);
}

uint8_t NoteWrap::bottom() const {
    return on_keyboard((int16_t)((int16_t)root() + low));
}

uint8_t NoteWrap::top() const {
    return on_keyboard((int16_t)((int16_t)root() + low + range));
}

uint8_t NoteWrap::wrap(uint8_t note) const {
    const int16_t lo = bottom();
    const int16_t hi = top();
    int16_t pitch = note;
    while (pitch > hi && pitch - 12 >= lo) pitch = (int16_t)(pitch - 12);
    while (pitch < lo && pitch + 12 <= hi) pitch = (int16_t)(pitch + 12);
    // A pitch class the range has no place for.
    if (pitch > hi || pitch < lo) return DROPPED;
    return (uint8_t)pitch;
}

void NoteWrap::process(BusManager& bus, uint32_t){
    // The root first, so a root and a note arriving in the same pass agree.
    const uint8_t rn = bus.note_count(root_in);
    for (uint8_t i = 0; i < rn; i++){
        const MidiEvent e = bus.note_read(root_in, i);
        if (is_note_on(e)) played_root = e.data1;
    }

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);
            continue;
        }
        if (!is_note_on(e)){
            bus.note_write(out, readdressed(e, channel));
            continue;
        }
        const uint8_t note = wrap(e.data1);
        if (note == DROPPED) continue;                 // and its note-off with it
        // Two incoming pitches an octave apart wrap to the same note. The
        // ledger keys on the source note, so each still gets its own release.
        sounding.emit(bus, out, e.data1, note, e.data2, out_channel(channel, e.channel));
    }
}

void NoteWrap::silence(BusManager& bus){
    sounding.release_all(bus, out);
}

bool NoteWrap::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_OCTAVE:
            if (value > KEY_MAX_OCTAVE) return false;
            octave = value;
            return true;
        case P_LOW:
            if (value < PARAM_CENTRE - MAX_LOW || value > PARAM_CENTRE + MAX_LOW) return false;
            low = param_centred(value);
            return true;
        case P_RANGE:
            if (value == 0 || value > MAX_RANGE) return false;
            range = value;
            return true;
        case P_CHANNEL:
            if (value > 16) return false;
            channel = value;
            return true;
        default:
            return false;
    }
}

uint8_t NoteWrap::get_param(uint16_t index) const {
    switch (index){
        case P_OCTAVE:  return octave;
        case P_LOW:     return param_centred_byte(low);
        case P_RANGE:    return range;
        case P_CHANNEL: return channel;
        default: return 0;
    }
}
