#include "algorithm/midi/probability.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "clock/transport_edge.h"

static const Domain IN[3] = {Domain::Note, Domain::Gate, Domain::Gate};
static const Domain OUT[2] = {Domain::Note, Domain::Gate};

// One group, labelled, because all four controls are one mechanism and an
// editor sorting them by name would put `ratio` and `condition` under
// "other" (app/src/views.js).
static const ParamGroup GROUPS[1] = {
    {0, 1, TrigCondition::N_PARAMS, TrigCondition::PARAMS, "chance"},
};

static const char* const IN_NAMES[3] = {"notes in", "fill", "nei"};
static const char* const OUT_NAMES[2] = {"notes out", "passed"};

const AlgorithmDescriptor Probability::descriptor = {
    ALGO_PROBABILITY, "Probability", 3, 1, 2, TrigCondition::N_PARAMS, IN, OUT,
    sizeof(Probability), false, construct_node<Probability>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Lets each note through under a chance, a ratio and a condition, and keeps its note-off with it.",
    CATEGORY_MIDI };

bool Probability::set_param(uint16_t index, uint8_t value){
    return condition.set_param(index, value);
}

uint8_t Probability::get_param(uint16_t index) const {
    return condition.get_param(index);
}

Probability::Probability(const NodeConfig& config) :
    in(config.in_bus[0]),
    fill_in(config.in_bus[1]),
    nei_in(config.in_bus[2]),
    out(config.out_bus[0]),
    passed_out(config.out_bus[1]),
    condition(config.params),
    sounding()
{}

void Probability::process(BusManager& bus, uint32_t){
    const bool fill = (fill_in == NO_BUS) ? false : bus.gate_read(fill_in);
    const bool nei = (nei_in == NO_BUS) ? false : bus.gate_read(nei_in);

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
        if (!condition.evaluate(fill, nei)) continue;
        sounding.emit(bus, out, e.data1, e.data1, e.data2, e.channel);
    }

    // Latched, not a pulse: the neighbour reading this has to see the last
    // decision whether or not it is clocked in the same pass as this one.
    if (passed_out != NO_BUS && condition.passed()) bus.gate_write(passed_out, true);
}

// A start puts the count back on the downbeat, so `first` and a ratio mean
// the same thing every time the pattern is played from the top. A continue
// is the message that means "where we left off" and leaves it alone.
void Probability::transport_event(BusManager&, uint8_t edges){
    if (edges & TRANSPORT_START) condition.restart();
}

void Probability::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
