#include "algorithm/sequencer/sequencers.h"
#include "algorithm/sequencer/euclid.h"
#include "node/registry.h"

static const Domain IN2[2] = {Domain::Gate, Domain::Gate};
static const Domain IN3[3] = {Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Gate};

// Parameter descriptors (#20). Every gate sequencer shares the same header
// and the same per-step probability run; the four differ only in params[3..7].
// Each descriptor is therefore the shared header, the subclass's own block
// padded out to params[7] with reserved bytes, and the shared probability
// run - so the table covers every index and an editor is never handed a
// parameter it has no description for.

static const ParamDescriptor STEP_PATTERN[4] = {
    {"steps 1-8",   0, 255, 0, PARAM_BITFIELD, nullptr},
    {"steps 9-16",  0, 255, 0, PARAM_BITFIELD, nullptr},
    {"steps 17-24", 0, 255, 0, PARAM_BITFIELD, nullptr},
    {"steps 25-32", 0, 255, 0, PARAM_BITFIELD, nullptr},
};

static const ParamDescriptor EUCLID_PARAMS[2] = {
    {"pulses",   0, MAX_SEQUENCE_LEN,     0, PARAM_NUMBER, nullptr},
    {"rotation", 0, MAX_SEQUENCE_LEN - 1, 0, PARAM_NUMBER, nullptr},
};

static const ParamDescriptor RANDOM_PARAMS[2] = {
    {"density", 1, 100, 50, PARAM_PERCENT, nullptr},
    {"seed",    0, 255, 0,  PARAM_NUMBER,  nullptr},
};

// The shared header, the subclass's own params[3..7] (padded out with
// reserved bytes so the table covers every index), then the shared
// probability run.
static const ParamGroup METRONOME_GROUPS[3] = {
    {0, 1, 3, GateSequencer::HEADER},
    {3, 5, 1, GateSequencer::RESERVED},
    {GateSequencer::PROBABILITY_BASE, MAX_SEQUENCE_LEN, 1, GateSequencer::PROBABILITY},
};

static const ParamGroup STEP_GROUPS[4] = {
    {0, 1, 3, GateSequencer::HEADER},
    {3, 1, 4, STEP_PATTERN},
    {7, 1, 1, GateSequencer::RESERVED},
    {GateSequencer::PROBABILITY_BASE, MAX_SEQUENCE_LEN, 1, GateSequencer::PROBABILITY},
};

static const ParamGroup EUCLID_GROUPS[4] = {
    {0, 1, 3, GateSequencer::HEADER},
    {3, 1, 2, EUCLID_PARAMS},
    {5, 3, 1, GateSequencer::RESERVED},
    {GateSequencer::PROBABILITY_BASE, MAX_SEQUENCE_LEN, 1, GateSequencer::PROBABILITY},
};

static const ParamGroup RANDOM_GROUPS[4] = {
    {0, 1, 3, GateSequencer::HEADER},
    {3, 1, 2, RANDOM_PARAMS},
    {5, 3, 1, GateSequencer::RESERVED},
    {GateSequencer::PROBABILITY_BASE, MAX_SEQUENCE_LEN, 1, GateSequencer::PROBABILITY},
};

// Every gate sequencer takes the per-step probability block at params[8..],
// so n_params is the same for all four whatever their own params[3..7] use.
// Inlet and outlet names, so a patch says what each connection *means*
// rather than "in 0" and "in 1". Reset is the second inlet in every
// sequencer, which is the shape being able to read the names makes visible.
static const char* const SEQ_IN_NAMES[2] = {"advance", "reset"};
static const char* const RANDOM_IN_NAMES[3] = {"advance", "reset", "shred"};
static const char* const SEQ_OUT_NAMES[1] = {"trigger"};

const AlgorithmDescriptor Metronome::descriptor = {
    ALGO_METRONOME, "Metronome", 2, 1, 1, GateSequencer::PARAM_COUNT, IN2, OUT, sizeof(Metronome), false, construct_node<Metronome>,
    METRONOME_GROUPS, 3, SEQ_IN_NAMES, SEQ_OUT_NAMES,
    "One trigger per advance edge. A GateSequencer of length one: the divider sets the rate." };

const AlgorithmDescriptor StepSequencer::descriptor = {
    ALGO_STEP_SEQ, "StepSequencer", 2, 1, 1, GateSequencer::PARAM_COUNT, IN2, OUT, sizeof(StepSequencer), false, construct_node<StepSequencer>,
    STEP_GROUPS, 4, SEQ_IN_NAMES, SEQ_OUT_NAMES,
    "A pattern of on/off steps, clicked in the grid below. Steps past the length are kept." };

const AlgorithmDescriptor EuclidianSequencer::descriptor = {
    ALGO_EUCLID_SEQ, "EuclidianSequencer", 2, 1, 1, GateSequencer::PARAM_COUNT, IN2, OUT, sizeof(EuclidianSequencer), false, construct_node<EuclidianSequencer>,
    EUCLID_GROUPS, 4, SEQ_IN_NAMES, SEQ_OUT_NAMES,
    "Bjorklund: spreads \"pulses\" evenly over \"steps\", plus a rotation." };

const AlgorithmDescriptor RandomSequencer::descriptor = {
    ALGO_RANDOM_SEQ, "RandomSequencer", 3, 1, 1, GateSequencer::PARAM_COUNT, IN3, OUT, sizeof(RandomSequencer), false, construct_node<RandomSequencer>,
    RANDOM_GROUPS, 4, RANDOM_IN_NAMES, SEQ_OUT_NAMES,
    "A random pattern at a density, held until the shred inlet draws a new one." };

// StepSequencer --------------------------------------------------------------

StepSequencer::StepSequencer(const NodeConfig& config) :
    GateSequencer(config, 8), bits(0)
{
    for (uint8_t i = 0; i < 4; i++) bits |= (uint32_t)config.params[3 + i] << (8u * i);
}

bool StepSequencer::set_param(uint16_t index, uint8_t value){
    if (index >= 3 && index <= 6){
        const uint8_t shift = (uint8_t)(8u * (index - 3u));
        bits = (bits & ~((uint32_t)0xFFu << shift)) | ((uint32_t)value << shift);
        return true;
    }
    return GateSequencer::set_param(index, value);
}

uint8_t StepSequencer::get_param(uint16_t index) const {
    if (index >= 3 && index <= 6) return (uint8_t)(bits >> (8u * (index - 3u)));
    return GateSequencer::get_param(index);
}

// EuclidianSequencer ---------------------------------------------------------

EuclidianSequencer::EuclidianSequencer(const NodeConfig& config) :
    GateSequencer(config, 8), bits(0),
    k(config.params[3]), rot(config.params[4])
{
    derive();
}

void EuclidianSequencer::derive(){
    bits = rotate_pattern(euclidean_pattern(k, length()), length(), rot);
}

bool EuclidianSequencer::set_param(uint16_t index, uint8_t value){
    // A no-op set is free: a knob sweep is ~100 messages a second and
    // Bjorklund should run once per distinct value, not once per message.
    if (index == 3){
        if (value > MAX_SEQUENCE_LEN) return false;
        if (value == k) return true;
        k = value;
        derive();
        return true;
    }
    if (index == 4){
        if (value >= MAX_SEQUENCE_LEN) return false;
        if (value == rot) return true;
        rot = value;
        derive();
        return true;
    }
    return GateSequencer::set_param(index, value);   // length re-derives via on_length_changed
}

uint8_t EuclidianSequencer::get_param(uint16_t index) const {
    if (index == 3) return k;
    if (index == 4) return rot;
    return GateSequencer::get_param(index);
}

// RandomSequencer ------------------------------------------------------------

RandomSequencer::RandomSequencer(const NodeConfig& config) :
    GateSequencer(config, 8), bits(0),
    density(config.params[3] ? config.params[3] : 50),
    seed_offset(config.params[4])
{
    rng.reseed(entropy::seed() + seed_offset);
    shred();
}

void RandomSequencer::shred(){
    bits = 0;
    for (uint8_t i = 0; i < length(); i++){
        if (rng.chance(density)) bits |= (uint32_t)1u << i;
    }
}

bool RandomSequencer::set_param(uint16_t index, uint8_t value){
    if (index == 3){
        if (value > 100) return false;
        density = value ? value : 50;
        return true;
    }
    if (index == 4){
        if (value == seed_offset) return true;
        seed_offset = value;
        rng.reseed(entropy::seed() + seed_offset);
        return true;
    }
    return GateSequencer::set_param(index, value);
}

uint8_t RandomSequencer::get_param(uint16_t index) const {
    if (index == 3) return density;
    if (index == 4) return seed_offset;
    return GateSequencer::get_param(index);
}
