#include "algorithm/midi/chord.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[7] = {
    {"voices",     0, Chord::MAX_INTERVALS, 0, PARAM_NUMBER, nullptr},
    {"interval 1", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 2", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 3", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 4", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 5", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 6", 0, 255, 0, PARAM_SIGNED, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 7, PARAMS}};

const AlgorithmDescriptor Chord::descriptor = {
    ALGO_CHORD, "Chord", 1, 1, 1, 7, IN, OUT, sizeof(Chord), false, construct_node<Chord>,
    GROUPS, 1 };

// The voicing can move under a held chord: every voice already in the air is
// released from the ledger at the pitch it was emitted at, so re-voicing
// changes what the *next* note-on plays and strands nothing.
bool Chord::set_param(uint16_t index, uint8_t value){
    if (index == 0){
        if (value > MAX_INTERVALS) return false;
        n_intervals = value;
        return true;
    }
    if (index >= 1 && index <= MAX_INTERVALS){
        intervals[index - 1] = (int8_t)value;
        return true;
    }
    return false;
}

uint8_t Chord::get_param(uint16_t index) const {
    if (index == 0) return n_intervals;
    if (index >= 1 && index <= MAX_INTERVALS) return (uint8_t)intervals[index - 1];
    return 0;
}

Chord::Chord(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    n_intervals(config.params[0] > MAX_INTERVALS ? MAX_INTERVALS : config.params[0]),
    intervals(), sounding()
{
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) intervals[i] = (int8_t)config.params[i + 1];
}

void Chord::process(BusManager& bus, uint32_t){
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
        sounding.emit(bus, out, e.data1, e.data1, e.data2, e.channel);   // the root
        for (uint8_t v = 0; v < n_intervals; v++){
            const int16_t note = (int16_t)e.data1 + intervals[v];
            if (note < 0 || note > 127) continue;
            if (note == e.data1) continue;                                // no unisons
            sounding.emit(bus, out, e.data1, (uint8_t)note, e.data2, e.channel);
        }
    }
}

void Chord::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
