#include "algorithm/midi/probability.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[2] = {Domain::Note, Domain::Gate};
static const Domain OUT[3] = {Domain::Note, Domain::Note, Domain::Gate};

// One group, labelled, because all three controls are one mechanism and an
// editor sorting them by name would put `condition` under "other"
// (app/src/views.js).
static const ParamGroup GROUPS[1] = {
    {0, 1, TrigCondition::N_PARAMS, TrigCondition::PARAMS, "chance"},
};

static const char* const IN_NAMES[2] = {"notes in", "reset"};
static const char* const OUT_NAMES[3] = {"notes out", "dropped", "decision"};

const AlgorithmDescriptor Probability::descriptor = {
    ALGO_PROBABILITY, "Probability", 2, 1, 3, TrigCondition::N_PARAMS, IN, OUT,
    sizeof(Probability), false, construct_node<Probability>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Lets each note through under a chance and a condition, and keeps its note-off with it.",
    CATEGORY_MIDI };

bool Probability::set_param(uint16_t index, uint8_t value){
    return condition.set_param(index, value);
}

uint8_t Probability::get_param(uint16_t index) const {
    return condition.get_param(index);
}

Probability::Probability(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    dropped_out(config.out_bus[1]),
    decision_out(config.out_bus[2]),
    reset_in(config.in_bus[1]),
    condition(config.params),
    sounding(),
    refused()
{}

void Probability::process(BusManager& bus, uint32_t){
    // Before the notes, so a reset and a note-on in one pass play the first
    // event of the new count rather than the last of the old one.
    if (reset_in.rising(bus)) condition.reset();

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            // Both ledgers, not one or the other: the note-off goes wherever
            // its note-on went, and a pitch that is sounding on both sides at
            // once - retriggered while it was held - is released on both
            // rather than left hanging on the side that did not match.
            sounding.release(bus, out, e.data1);
            refused.release(bus, dropped_out, e.data1);
            continue;
        }
        if (!is_note_on(e)){
            bus.note_write(out, e);
            continue;
        }
        if (!condition.evaluate()){
            // Unrecorded when nothing is patched there: a ledger that filled
            // up with notes nobody can hear would refuse the ones that can.
            if (dropped_out != NO_BUS)
                refused.emit(bus, dropped_out, e.data1, e.data1, e.data2, e.channel);
            continue;
        }
        sounding.emit(bus, out, e.data1, e.data1, e.data2, e.channel);
    }

    // Latched, not a pulse: a reader clocked in some other pass has to see
    // the last decision rather than nothing.
    if (decision_out != NO_BUS && condition.decision()) bus.gate_write(decision_out, true);
}

void Probability::silence(BusManager& bus){
    sounding.release_all(bus, out);
    refused.release_all(bus, dropped_out);
}
