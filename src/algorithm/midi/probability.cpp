#include "algorithm/midi/probability.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor Probability::descriptor = {
    ALGO_PROBABILITY, "Probability", 1, 1, 1, 2, IN, OUT, sizeof(Probability), false, construct_node<Probability> };

Probability::Probability(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    percent(config.params[0] ? config.params[0] : 100),
    rng(entropy::seed() + config.params[1]),
    sounding()
{}

void Probability::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);     // silent if the on was dropped
            continue;
        }
        if (!is_note_on(e)){
            bus.note_write(out, e);
            continue;
        }
        if (!rng.chance(percent)) continue;
        sounding.emit(bus, out, e.data1, e.data1, e.data2, e.channel);
    }
}
