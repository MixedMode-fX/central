#include "algorithm/midi/transpose.h"
#include "node/registry.h"
#include "hal/midi_types.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor Transpose::descriptor = {
    ALGO_TRANSPOSE, "Transpose", 1, 1, 1, 1, IN, OUT, sizeof(Transpose), false, construct_node<Transpose> };

Transpose::Transpose(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    semitones((int8_t)config.params[0])
{}

void Transpose::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        MidiEvent e = bus.note_read(in, i);
        if (e.type == MIDI_NOTE_ON || e.type == MIDI_NOTE_OFF){
            const int16_t shifted = (int16_t)e.data1 + semitones;
            if (shifted < 0 || shifted > 127) continue;
            e.data1 = (uint8_t)shifted;
        }
        bus.note_write(out, e);
    }
}
