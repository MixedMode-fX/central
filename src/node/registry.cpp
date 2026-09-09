#include "node/registry.h"
#include "algorithm/logic/gates.h"
#include "algorithm/switch/sustain.h"
#include "algorithm/midi/gate_to_note.h"
#include "algorithm/midi/transpose.h"
#include "algorithm/midi/arpeggiator.h"
#include "algorithm/midi/note_priority.h"
#include "algorithm/midi/velocity_curve.h"
#include "algorithm/midi/chord.h"
#include "algorithm/midi/quantise.h"
#include "algorithm/midi/probability.h"
#include "algorithm/clock/clock_div.h"

// The compile-time table. Every algorithm's code is always resident; this is
// what a patch selects an instance from.
static const AlgorithmDescriptor* const TABLE[] = {
    &LogicNot::descriptor,
    &LogicAND::descriptor,
    &LogicNAND::descriptor,
    &LogicOR::descriptor,
    &LogicNOR::descriptor,
    &LogicXOR::descriptor,
    &LogicXNOR::descriptor,
    &Sustain::descriptor,
    &GateToNote::descriptor,
    &Transpose::descriptor,
    &Arpeggiator::descriptor,
    &ClockDiv::descriptor,
    &NotePriority::descriptor,
    &VelocityCurve::descriptor,
    &Chord::descriptor,
    &Quantise::descriptor,
    &Probability::descriptor,
};

static const uint8_t TABLE_SIZE = sizeof(TABLE) / sizeof(TABLE[0]);

const AlgorithmDescriptor* registry::find(uint8_t algorithm_id){
    for (uint8_t i = 0; i < TABLE_SIZE; i++){
        if (TABLE[i]->id == algorithm_id) return TABLE[i];
    }
    return nullptr;
}

uint8_t registry::count(){ return TABLE_SIZE; }

const AlgorithmDescriptor* registry::at(uint8_t index){
    return index < TABLE_SIZE ? TABLE[index] : nullptr;
}

ConfigError registry::validate(const NodeConfig& config){
    const AlgorithmDescriptor* d = find(config.algorithm_id);
    if (d == nullptr) return CONFIG_UNKNOWN_ALGORITHM;
    for (uint8_t i = 0; i < d->n_in && i < MAX_IN; i++){
        const uint8_t bus = config.in_bus[i];
        if (bus == NO_BUS){
            if (i < d->min_in) return CONFIG_INLET_NOT_CONNECTED;
            continue;
        }
        if (bus >= bus_count(d->in_domain[i])) return CONFIG_INLET_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < d->n_out && i < MAX_OUT; i++){
        const uint8_t bus = config.out_bus[i];
        if (bus == NO_BUS || bus >= bus_count(d->out_domain[i])) return CONFIG_OUTLET_OUT_OF_RANGE;
    }
    return CONFIG_OK;
}
