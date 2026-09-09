#include "algorithm/midi/note_priority.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const MODE_NAMES[3] = {"lowest", "highest", "latest"};
static const ParamDescriptor PARAMS[1] = {
    {"mode", 0, 2, 0, PARAM_ENUM, MODE_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, 1, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"note out"};

const AlgorithmDescriptor NotePriority::descriptor = {
    ALGO_NOTE_PRIORITY, "NotePriority", 1, 1, 1, 1, IN, OUT, sizeof(NotePriority), false, construct_node<NotePriority>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Many notes in, one out: the lowest, the highest or the latest held note wins." };

// The new winner is taken on the next pass by follow(), which releases the
// old voice from the ledger, so the mode can move under a held chord.
bool NotePriority::set_param(uint16_t index, uint8_t value){
    if (index != 0 || value > PRIORITY_LATEST) return false;
    mode = value;
    return true;
}

uint8_t NotePriority::get_param(uint16_t index) const {
    return index == 0 ? mode : 0;
}

NotePriority::NotePriority(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    mode(config.params[0]),
    playing(HeldNotes::NONE),
    held(), sounding()
{}

uint8_t NotePriority::winner() const {
    switch (mode){
        case PRIORITY_HIGH:   return held.highest();
        case PRIORITY_LATEST: return held.latest();
        default:              return held.lowest();
    }
}

void NotePriority::follow(BusManager& bus){
    const uint8_t want = winner();
    if (want == playing) return;
    if (playing != HeldNotes::NONE){
        sounding.release(bus, out, playing);
        playing = HeldNotes::NONE;
    }
    if (want == HeldNotes::NONE) return;
    for (uint8_t i = 0; i < held.count(); i++){
        const HeldNote& h = held.at(i);
        if (h.note != want) continue;
        if (sounding.emit(bus, out, want, want, h.velocity, h.channel)) playing = want;
        return;
    }
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
            bus.note_write(out, e);          // CC, bend and the rest pass through
            continue;
        }
        follow(bus);
    }
}

void NotePriority::silence(BusManager& bus){
    sounding.release_all(bus, out);
    playing = HeldNotes::NONE;
}
