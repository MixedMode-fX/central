#include "algorithm/midi/quantise.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/scale.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor Quantise::descriptor = {
    ALGO_QUANTISE, "Quantise", 2, 1, 1, 2, IN, OUT, sizeof(Quantise), false, construct_node<Quantise> };

Quantise::Quantise(const NodeConfig& config) :
    in(config.in_bus[0]),
    root_in(config.in_bus[1]),
    out(config.out_bus[0]),
    scale(config.params[0]),
    root((uint8_t)(config.params[1] % 12u)),
    sounding()
{}

void Quantise::process(BusManager& bus, uint32_t){
    // The root first, so a root and a note arriving in the same pass agree.
    if (root_in != NO_BUS){
        const uint8_t rn = bus.note_count(root_in);
        for (uint8_t i = 0; i < rn; i++){
            const MidiEvent e = bus.note_read(root_in, i);
            if (is_note_on(e)) root = (uint8_t)(e.data1 % 12u);
        }
    }

    const uint16_t mask = scale_mask(scale);
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);
            continue;
        }
        if (!is_note_on(e)){
            bus.note_write(out, e);
            continue;
        }
        const uint8_t snapped = scale_quantise(e.data1, root, mask);
        // Two incoming pitches can snap to the same tone. The ledger keys on
        // the source note, so each of them still gets its own release.
        sounding.emit(bus, out, e.data1, snapped, e.data2, e.channel);
    }
}
