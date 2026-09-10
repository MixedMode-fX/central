#include "algorithm/midi/probability.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[2] = {
    {"chance", 1, 100, 100, PARAM_PERCENT, nullptr},
    {"seed",   0, 255, 0,   PARAM_NUMBER,  nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 2, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Probability::descriptor = {
    ALGO_PROBABILITY, "Probability", 1, 1, 1, 2, IN, OUT, sizeof(Probability), false, construct_node<Probability>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Lets each note through with a chance, and keeps its note-off with it.",
    CATEGORY_MIDI };

// The odds move freely: the pass/drop decision is taken on the note-on and
// remembered, so a note already passed is always released whatever the
// chance has become. Writing the seed re-seeds, which is the point of a knob
// on it - two nodes at the same odds are made to disagree from the host.
bool Probability::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: percent = value ? value : 100; return true;
        case 1:
            if (value == seed_offset) return true;
            seed_offset = value;
            rng.reseed(entropy::seed() + seed_offset);
            return true;
        default: return false;
    }
}

uint8_t Probability::get_param(uint16_t index) const {
    switch (index){
        case 0: return percent;
        case 1: return seed_offset;
        default: return 0;
    }
}

Probability::Probability(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    percent(config.params[0] ? config.params[0] : 100),
    seed_offset(config.params[1]),
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

void Probability::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
