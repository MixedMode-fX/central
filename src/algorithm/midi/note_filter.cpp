#include "algorithm/midi/note_filter.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const PASS_NAMES[3] = {"all", "notes", "controls"};

// `high` and `vel max` are the two parameters whose default is not their
// minimum, so they are the two a stored zero would turn into the top of the
// range rather than the bottom. Both are given a minimum of 1 for that
// reason: #21 scales a controller onto [min, max] and never produces 0 for a
// parameter whose min is 1, so a knob sweeping the window shut cannot fall
// off the end and spring back open. What that costs is a window whose top is
// note 0 - which is a window holding one note nobody plays.
static const ParamDescriptor PARAMS[NoteFilter::N_PARAMS] = {
    {"channel", 0, 16,  0,   PARAM_CHANNEL, nullptr},
    {"low",     0, 127, 0,   PARAM_PITCH,   nullptr},
    {"high",    1, 127, 127, PARAM_PITCH,   nullptr},
    {"vel min", 1, 127, 1,   PARAM_NUMBER,  nullptr},
    {"vel max", 1, 127, 127, PARAM_NUMBER,  nullptr},
    {"pass",    0, 2,   0,   PARAM_ENUM,    PASS_NAMES},
    {"not channel", 0, 1, 0, PARAM_BOOL,  nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, NoteFilter::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor NoteFilter::descriptor = {
    ALGO_NOTE_FILTER, "NoteFilter", 1, 1, 1, NoteFilter::N_PARAMS, IN, OUT,
    sizeof(NoteFilter), false, construct_node<NoteFilter>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Passes only what matches a channel, a note range, a velocity window and a message type.",
    CATEGORY_MIDI };

NoteFilter::NoteFilter(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    channel(config.params[P_CHANNEL]),
    low(config.params[P_LOW]),
    high(config.params[P_HIGH] ? config.params[P_HIGH] : DEFAULT_HIGH),
    vel_min(config.params[P_VEL_MIN] ? config.params[P_VEL_MIN] : DEFAULT_VEL_MIN),
    vel_max(config.params[P_VEL_MAX] ? config.params[P_VEL_MAX] : DEFAULT_VEL_MAX),
    pass(config.params[P_PASS]),
    not_channel(config.params[P_NOT_CHANNEL] != 0),
    sounding()
{}

bool NoteFilter::passes(const MidiEvent& event) const {
    const bool on_channel = channel == ANY_CHANNEL || event.channel == channel;
    if (on_channel == not_channel) return false;
    if (is_note(event)){
        if (pass == PASS_CONTROLS) return false;
        if (event.data1 < low || event.data1 > high) return false;
        // A note-off never reaches here, so this is a note-on's velocity and
        // never the 0 a release carries.
        return event.data2 >= vel_min && event.data2 <= vel_max;
    }
    return pass != PASS_NOTES;
}

// The whole algorithm is the note-off rule: a release is never tested, it is
// looked up. Everything else here is a comparison.
void NoteFilter::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);   // silent if the on was dropped
            continue;
        }
        if (is_note_on(e)){
            if (passes(e)) sounding.emit(bus, out, e.data1, e.data1, e.data2, e.channel);
            continue;
        }
        if (passes(e)) bus.note_write(out, e);
    }
}

// A window that moves under a held note cannot strand it: the ledger is what
// releases, so a note already passed is always released at the pitch and on
// the channel it went out on. Nothing here re-derives a release.
bool NoteFilter::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_CHANNEL: if (value > 16) return false; channel = value; return true;
        case P_LOW:     if (value > 127) return false; low = value; return true;
        case P_HIGH:    high = value ? value : DEFAULT_HIGH; return true;
        case P_VEL_MIN: vel_min = value ? value : DEFAULT_VEL_MIN; return true;
        case P_VEL_MAX: vel_max = value ? value : DEFAULT_VEL_MAX; return true;
        case P_PASS:    if (value >= PASS_MODES) return false; pass = value; return true;
        case P_NOT_CHANNEL: not_channel = value != 0; return true;
        default: return false;
    }
}

uint8_t NoteFilter::get_param(uint16_t index) const {
    switch (index){
        case P_CHANNEL: return channel;
        case P_LOW:     return low;
        case P_HIGH:    return high;
        case P_VEL_MIN: return vel_min;
        case P_VEL_MAX: return vel_max;
        case P_PASS:    return pass;
        case P_NOT_CHANNEL: return not_channel ? 1 : 0;
        default: return 0;
    }
}

void NoteFilter::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
