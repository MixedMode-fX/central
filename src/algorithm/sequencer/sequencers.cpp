#include "algorithm/sequencer/sequencers.h"
#include "algorithm/sequencer/euclid.h"
#include "node/registry.h"

static const Domain IN2[2] = {Domain::Gate, Domain::Gate};
static const Domain IN3[3] = {Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Gate};

// Every gate sequencer takes the per-step probability block at params[8..],
// so n_params is the same for all four whatever their own params[3..7] use.
const AlgorithmDescriptor Metronome::descriptor = {
    ALGO_METRONOME, "Metronome", 2, 1, 1, GateSequencer::PARAM_COUNT, IN2, OUT, sizeof(Metronome), false, construct_node<Metronome> };

const AlgorithmDescriptor StepSequencer::descriptor = {
    ALGO_STEP_SEQ, "StepSequencer", 2, 1, 1, GateSequencer::PARAM_COUNT, IN2, OUT, sizeof(StepSequencer), false, construct_node<StepSequencer> };

const AlgorithmDescriptor EuclidianSequencer::descriptor = {
    ALGO_EUCLID_SEQ, "EuclidianSequencer", 2, 1, 1, GateSequencer::PARAM_COUNT, IN2, OUT, sizeof(EuclidianSequencer), false, construct_node<EuclidianSequencer> };

const AlgorithmDescriptor RandomSequencer::descriptor = {
    ALGO_RANDOM_SEQ, "RandomSequencer", 3, 1, 1, GateSequencer::PARAM_COUNT, IN3, OUT, sizeof(RandomSequencer), false, construct_node<RandomSequencer> };

StepSequencer::StepSequencer(const NodeConfig& config) :
    GateSequencer(config, 8), bits(0)
{
    for (uint8_t i = 0; i < 4; i++) bits |= (uint32_t)config.params[3 + i] << (8u * i);
    bits &= euclid_mask(length());
}

EuclidianSequencer::EuclidianSequencer(const NodeConfig& config) :
    GateSequencer(config, 8), bits(0)
{
    bits = rotate_pattern(euclidean_pattern(config.params[3], length()),
                          length(), config.params[4]);
}

RandomSequencer::RandomSequencer(const NodeConfig& config) :
    GateSequencer(config, 8), bits(0),
    density(config.params[3] ? config.params[3] : 50)
{
    rng.reseed(entropy::seed() + config.params[4]);
    shred();
}

void RandomSequencer::shred(){
    bits = 0;
    for (uint8_t i = 0; i < length(); i++){
        if (rng.chance(density)) bits |= (uint32_t)1u << i;
    }
}
