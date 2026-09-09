#include "algorithm/midi/transpose.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor Transpose::descriptor = {
    ALGO_TRANSPOSE, "Transpose", 1, 1, 1, 1, IN, OUT, sizeof(Transpose), false, construct_node<Transpose> };

Transpose::Transpose(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    semitones((int8_t)config.params[0]),
    sounding()
{}

void Transpose::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);      // as sent, not as configured
            continue;
        }
        if (is_note_on(e)){
            const int16_t shifted = (int16_t)e.data1 + semitones;
            if (shifted < 0 || shifted > 127) continue;   // dropped, off and all
            sounding.emit(bus, out, e.data1, (uint8_t)shifted, e.data2, e.channel);
            continue;
        }
        bus.note_write(out, e);
    }
}

void Transpose::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
