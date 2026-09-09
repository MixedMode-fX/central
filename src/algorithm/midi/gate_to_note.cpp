#include "algorithm/midi/gate_to_note.h"
#include "node/registry.h"
#include "hal/midi_types.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[3] = {
    {"note",     0, 127, 60,  PARAM_PITCH,   nullptr},
    {"velocity", 1, 127, 100, PARAM_NUMBER,  nullptr},
    {"channel",  1, 16,  1,   PARAM_CHANNEL, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 3, PARAMS}};

static const char* const IN_NAMES[1] = {"gate"};
static const char* const OUT_NAMES[1] = {"note out"};

const AlgorithmDescriptor GateToNote::descriptor = {
    ALGO_GATE_TO_NOTE, "GateToNote", 1, 1, 1, 3, IN, OUT, sizeof(GateToNote), false, construct_node<GateToNote>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A gate becomes a note: rising edge sends note on, falling edge sends note off." };

GateToNote::GateToNote(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    note(config.params[0] ? config.params[0] : 60),
    velocity(config.params[1] ? config.params[1] : 100),
    channel(config.params[2] ? config.params[2] : 1),
    sent_note(0), sent_channel(0),
    last(false)
{}

void GateToNote::process(BusManager& bus, uint32_t){
    const bool level = bus.gate_read(in);
    if (level == last) return;
    last = level;
    if (level){
        sent_note = note;
        sent_channel = channel;
        const MidiEvent on = {MIDI_NOTE_ON, sent_channel, sent_note, velocity};
        bus.note_write(out, on);
        return;
    }
    const MidiEvent off = {MIDI_NOTE_OFF, sent_channel, sent_note, 0};
    bus.note_write(out, off);
}

bool GateToNote::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: note = value & 0x7F; return true;
        case 1: velocity = value & 0x7F; return true;
        case 2: channel = value ? value : 1; return true;
        default: return false;
    }
}

uint8_t GateToNote::get_param(uint16_t index) const {
    switch (index){
        case 0: return note;
        case 1: return velocity;
        case 2: return channel;
        default: return 0;
    }
}

// The gate may still be high when the patch is swapped: the note-off it
// would have sent on the falling edge goes now.
void GateToNote::silence(BusManager& bus){
    if (!last) return;
    last = false;
    const MidiEvent e = {MIDI_NOTE_OFF, sent_channel, sent_note, 0};
    bus.note_write(out, e);
}
