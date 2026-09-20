#include "algorithm/midi/note_priority.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[NotePriority::N_PARAMS] = {
    {"mode", NOTE_PRIORITY_LOWEST, NOTE_PRIORITY_LATEST, NOTE_PRIORITY_LOWEST,
     PARAM_ENUM, PARAM_PRIORITY_NAMES},
    {"channel", 0, 16, 0, PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, NotePriority::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"note out"};

const AlgorithmDescriptor NotePriority::descriptor = {
    ALGO_NOTE_PRIORITY, "NotePriority", 1, 1, 1, NotePriority::N_PARAMS, IN, OUT,
    sizeof(NotePriority), false, construct_node<NotePriority>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Many notes in, one out: the lowest, the highest or the latest held note wins.",
    CATEGORY_MIDI };

// The new winner is taken on the next pass by follow(), which releases the
// old voice from the ledger, so the mode can move under a held chord.
bool NotePriority::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_MODE:
            if (value > PRIORITY_LATEST) return false;
            mode = value; return true;
        // The voice in the air was recorded with the channel it went out on,
        // so this may move while a key is down.
        case P_CHANNEL:
            if (value > 16) return false;
            channel = value; return true;
        default: return false;
    }
}

uint8_t NotePriority::get_param(uint16_t index) const {
    switch (index){
        case P_MODE:    return mode;
        case P_CHANNEL: return channel;
        default:        return 0;
    }
}

NotePriority::NotePriority(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    mode(config.params[P_MODE]),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    playing(HeldNotes::NONE),
    held(), sounding()
{}

void NotePriority::follow(BusManager& bus){
    const uint8_t want = held.winner((NotePriorityRule)mode);
    if (want == playing) return;
    if (playing != HeldNotes::NONE){
        sounding.release(bus, out, playing);
        playing = HeldNotes::NONE;
    }
    if (want == HeldNotes::NONE) return;
    const HeldNote* h = held.find(want);
    if (h == nullptr) return;
    if (sounding.emit(bus, out, want, want, h->velocity, out_channel(channel, h->channel)))
        playing = want;
}

void NotePriority::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_on(e)){
            HeldNote evicted = {0, 0, 0};
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
            if (did_evict){
                // The evicted note is no longer held, so it must not be left
                // sounding either.
                sounding.release(bus, out, evicted.note);
                if (playing == evicted.note) playing = HeldNotes::NONE;
            }
        } else if (is_note_off(e)){
            held.remove(e.data1);
        } else {
            bus.note_write(out, readdressed(e, channel));   // CC, bend and the rest
            continue;
        }
        follow(bus);
    }
}

void NotePriority::silence(BusManager& bus){
    sounding.release_all(bus, out);
    playing = HeldNotes::NONE;
}
