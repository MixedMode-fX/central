#include "algorithm/midi/note_buffer.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Note, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[NoteBuffer::N_PARAMS] = {
    {"channel", 0, 16, 0, PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, NoteBuffer::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"notes in", "reset"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor NoteBuffer::descriptor = {
    ALGO_NOTE_BUFFER, "NoteBuffer", 2, 1, 1, NoteBuffer::N_PARAMS, IN, OUT,
    sizeof(NoteBuffer), false, construct_node<NoteBuffer>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Holds every note that comes in until a reset edge releases them all.",
    CATEGORY_MIDI };

NoteBuffer::NoteBuffer(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    reset_in(config.in_buses[1]),
    held()
{}

void NoteBuffer::process(BusManager& bus, uint32_t){
    if (reset_in.rising(bus)) held.release_all(bus, out);

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)) continue;
        if (is_note_on(e)){
            if (!held.holds(e.data1))
                held.emit(bus, out, e.data1, e.data1, e.data2, out_channel(channel, e.channel));
            continue;
        }
        bus.note_write(out, readdressed(e, channel));
    }
}

void NoteBuffer::silence(BusManager& bus){
    held.release_all(bus, out);
}

// Each held note was recorded with the channel it went out on, so a new
// channel is heard on the next note and strands nothing.
bool NoteBuffer::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_CHANNEL: if (value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t NoteBuffer::get_param(uint16_t index) const {
    switch (index){
        case P_CHANNEL: return channel;
        default: return 0;
    }
}
