#include "algorithm/midi/channel.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[Channel::N_PARAMS] = {
    {"channel", 1, 16, 1, PARAM_CHANNEL, nullptr},
    {"count",   1, 16, 1, PARAM_NUMBER,  nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, Channel::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Channel::descriptor = {
    ALGO_CHANNEL, "Channel", 1, 1, 1, Channel::N_PARAMS, IN, OUT,
    sizeof(Channel), false, construct_node<Channel>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Sends a stream out on one channel, or spreads its notes over a span of them.",
    CATEGORY_MIDI };

Channel::Channel(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    first(config.params[P_CHANNEL] ? config.params[P_CHANNEL] : (uint8_t)1),
    count(config.params[P_COUNT] ? config.params[P_COUNT] : (uint8_t)1),
    next(0),
    sounding()
{}

uint8_t Channel::channel_at(uint8_t slot) const {
    return (uint8_t)(((first - 1u + slot) % N_MIDI_CHANNELS) + 1u);
}

bool Channel::busy(uint8_t midi_channel) const {
    for (uint8_t i = 0; i < sounding.count(); i++){
        if (sounding.at(i).channel == midi_channel) return true;
    }
    return false;
}

uint8_t Channel::allocate(){
    for (uint8_t i = 0; i < count; i++){
        const uint8_t slot = (uint8_t)((next + i) % count);
        const uint8_t midi_channel = channel_at(slot);
        if (busy(midi_channel)) continue;
        next = (uint8_t)((slot + 1u) % count);
        return midi_channel;
    }
    // Every channel in the span is sounding: there is no free voice to find,
    // so the cursor's own is the least recently started.
    const uint8_t midi_channel = channel_at(next);
    next = (uint8_t)((next + 1u) % count);
    return midi_channel;
}

void Channel::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);   // wherever the on went
            continue;
        }
        if (is_note_on(e)){
            const uint8_t midi_channel = allocate();
            sounding.emit(bus, out, e.data1, e.data1, e.data2, midi_channel);
            continue;
        }
        MidiEvent moved = e;
        moved.channel = first;
        bus.note_write(out, moved);
    }
}

// Both parameters may move under a held chord: the ledger releases on the
// channel it recorded, so nothing is stranded. The cursor is the one piece of
// state that has to follow, because a narrowed span would otherwise leave it
// pointing outside itself.
bool Channel::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_CHANNEL:
            if (value == 0 || value > N_MIDI_CHANNELS) return false;
            first = value;
            return true;
        case P_COUNT:
            if (value == 0 || value > N_MIDI_CHANNELS) return false;
            count = value;
            next = (uint8_t)(next % count);
            return true;
        default: return false;
    }
}

uint8_t Channel::get_param(uint16_t index) const {
    switch (index){
        case P_CHANNEL: return first;
        case P_COUNT:   return count;
        default: return 0;
    }
}

void Channel::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
