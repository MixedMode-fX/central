#include "algorithm/midi/gate_to_note.h"
#include "node/registry.h"
#include "hal/midi_types.h"

static const Domain IN[1] = {Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor GateToNote::descriptor = {
    ALGO_GATE_TO_NOTE, "GateToNote", 1, 1, 1, 3, IN, OUT, sizeof(GateToNote), false, construct_node<GateToNote> };

GateToNote::GateToNote(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    note(config.params[0] ? config.params[0] : 60),
    velocity(config.params[1] ? config.params[1] : 100),
    channel(config.params[2] ? config.params[2] : 1),
    last(false)
{}

void GateToNote::process(BusManager& bus, uint32_t){
    const bool level = bus.gate_read(in);
    if (level == last) return;
    last = level;
    const MidiEvent e = {(uint8_t)(level ? MIDI_NOTE_ON : MIDI_NOTE_OFF), channel, note, (uint8_t)(level ? velocity : 0)};
    bus.note_write(out, e);
}
