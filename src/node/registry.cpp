#include "node/registry.h"
#include "algorithm/logic/gates.h"
#include "algorithm/switch/sustain.h"
#include "algorithm/midi/gate_to_note.h"
#include "algorithm/midi/transpose.h"
#include "algorithm/midi/arpeggiator.h"
#include "algorithm/midi/note_priority.h"
#include "algorithm/midi/velocity_curve.h"
#include "algorithm/midi/chord.h"
#include "algorithm/midi/note_quantise.h"
#include "algorithm/midi/probability.h"
#include "algorithm/midi/midi_to_cv.h"
#include "algorithm/clock/clock_div.h"
#include "algorithm/clock/metronome.h"
#include "algorithm/sequencer/sequencers.h"
#include "algorithm/sequencer/note_sequencer.h"
#include "algorithm/sequencer/drum_sequencer.h"
#include "algorithm/util/gate_hold.h"
#include "algorithm/modulator/lfo.h"
#include "algorithm/modulator/sample_hold.h"
#include "algorithm/modulator/slew.h"
#include "algorithm/midi/cv_to_note.h"
#include "algorithm/util/cv_to_gate.h"
#include "algorithm/modulator/turing.h"
#include "algorithm/midi/harmony.h"
#include "algorithm/sequencer/automaton.h"
#include "algorithm/midi/note_delay.h"
#include "algorithm/midi/voicer.h"
#include "algorithm/midi/mirror.h"
#include "algorithm/midi/tonnetz.h"
#include "algorithm/midi/note_filter.h"
#include "algorithm/midi/channel.h"
#include "algorithm/midi/key.h"

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
    &Metronome::descriptor,
    &NotePriority::descriptor,
    &VelocityCurve::descriptor,
    &Chord::descriptor,
    &NoteQuantise::descriptor,
    &Probability::descriptor,
    &StepSequencer::descriptor,
    &EuclidianSequencer::descriptor,
    &RandomSequencer::descriptor,
    &NoteSequencer::descriptor,
    &PolySequencer::descriptor,
    &DrumSeqGate::descriptor,
    &DrumSeqMidi::descriptor,
    &GateHold::descriptor,
    &Lfo::descriptor,
    &SampleHold::descriptor,
    &Slew::descriptor,
    &MidiToCv::descriptor,
    &CvToNote::descriptor,
    &CvToGate::descriptor,
    &Turing::descriptor,
    &Harmony::descriptor,
    &Automaton::descriptor,
    &NoteDelay::descriptor,
    &Voicer::descriptor,
    &Mirror::descriptor,
    &Tonnetz::descriptor,
    &NoteFilter::descriptor,
    &Channel::descriptor,
    &Key::descriptor,
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

const ParamDescriptor* registry::param(const AlgorithmDescriptor& algorithm, uint16_t index){
    if (index >= algorithm.n_params) return nullptr;
    return param_lookup(algorithm.param_groups, algorithm.n_param_groups, index);
}

const ParamDescriptor* registry::param(uint8_t algorithm_id, uint16_t index){
    const AlgorithmDescriptor* d = find(algorithm_id);
    return d == nullptr ? nullptr : param(*d, index);
}

bool registry::param_in_range(const ParamDescriptor& d, uint8_t value){
    // Zero always means "the default" (param.h), so a zeroed preset is valid
    // whatever the parameter's real minimum is.
    if (value == 0) return true;
    return value >= d.min && value <= d.max;
}

// Set by validate() when it returns CONFIG_PARAM_OUT_OF_RANGE, so the caller
// - the console, #11's SysEx reply - can say *which* parameter was wrong
// rather than only that one was.
static uint16_t bad_param = 0;

uint16_t registry::last_bad_param(){ return bad_param; }

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
    // An outlet left at NO_BUS is unused: the node's writes to it go nowhere
    // (BusManager ignores the index). A drum sequencer with eight lanes and
    // three jacks patched is the normal case, not an error.
    for (uint8_t i = 0; i < d->n_out && i < MAX_OUT; i++){
        const uint8_t bus = config.out_bus[i];
        if (bus != NO_BUS && bus >= bus_count(d->out_domain[i])) return CONFIG_OUTLET_OUT_OF_RANGE;
    }
    // Parameters (#20). Bytes beyond n_params are not checked: an algorithm
    // uses the first few and leaves the rest zero, and #11 exploits that by
    // not sending trailing zeros.
    for (uint16_t i = 0; i < d->n_params && i < N_PARAM; i++){
        const ParamDescriptor* p = param(*d, i);
        if (p == nullptr) continue;              // no group covers it: reserved
        if (!param_in_range(*p, config.params[i])){
            bad_param = i;
            return CONFIG_PARAM_OUT_OF_RANGE;
        }
    }
    return CONFIG_OK;
}
