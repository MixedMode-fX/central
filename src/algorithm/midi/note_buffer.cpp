#include "algorithm/midi/note_buffer.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Note, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const char* const MODE_NAMES[2] = {"latch", "sustain"};
static const ParamDescriptor PARAMS[NoteBuffer::N_PARAMS] = {
    {"channel", 0, 16, 0, PARAM_CHANNEL_OUT, nullptr},
    {"mode",    0, 1,  0, PARAM_ENUM,        MODE_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, NoteBuffer::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"notes in", "reset / hold"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor NoteBuffer::descriptor = {
    ALGO_NOTE_BUFFER, "NoteBuffer", 2, 1, 1, NoteBuffer::N_PARAMS, IN, OUT,
    sizeof(NoteBuffer), false, construct_node<NoteBuffer>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Holds every note played until a reset edge, or, in sustain, while a gate is held.",
    CATEGORY_MIDI };

NoteBuffer::NoteBuffer(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    gate(config.in_buses[1]),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    mode(config.params[P_MODE] > MODE_SUSTAIN ? MODE_LATCH : config.params[P_MODE]),
    gate_was(false),
    down{0, 0, 0, 0},
    held()
{}

void NoteBuffer::set_key(uint8_t note, bool is_down){
    const uint32_t bit = 1u << (note & 31);
    if (is_down) down[note >> 5] |= bit;
    else down[note >> 5] &= ~bit;
}

void NoteBuffer::process(BusManager& bus, uint32_t){
    const bool level = gate.any() && bus.gate_read(gate);
    const bool rising = level && !gate_was;
    gate_was = level;
    const bool sustain = mode == MODE_SUSTAIN;

    if (!sustain && rising) held.release_all(bus, out);
    // Pedal up: whatever it was keeping, and nothing's key still keeps, goes.
    // Swept every pass rather than on the falling edge, so a switch into
    // sustain with the pedal up strands nothing the latch was holding.
    if (sustain && !level){
        uint8_t i = held.count();
        while (i > 0){
            i--;
            const uint8_t source = held.at(i).source;
            if (!key_down(source)) held.release(bus, out, source);
        }
    }

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            set_key(e.data1 & 0x7F, false);
            if (sustain && !level) held.release(bus, out, e.data1);
            continue;
        }
        if (is_note_on(e)){
            set_key(e.data1 & 0x7F, true);
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
        // Into sustain, the next pass with the pedal up releases what the
        // latch held; into latch, what the pedal held stays until a reset.
        case P_MODE: if (value > MODE_SUSTAIN) return false; mode = value; return true;
        default: return false;
    }
}

uint8_t NoteBuffer::get_param(uint16_t index) const {
    switch (index){
        case P_CHANNEL: return channel;
        case P_MODE: return mode;
        default: return 0;
    }
}
