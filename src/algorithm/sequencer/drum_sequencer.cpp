#include "algorithm/sequencer/drum_sequencer.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain GATE_OUT[DRUM_SEQ_LANES] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };
static const Domain NOTE_OUT[1] = {Domain::Note};

static_assert(DRUM_SEQ_LANES == 8, "GATE_OUT lists one domain per lane");
static_assert(DRUM_SEQ_LANES <= MAX_OUT, "DrumSeqGate has one outlet per lane");
static_assert(DrumSeqMidi::PARAM_COUNT <= N_PARAM, "DrumSeqMidi's grid does not fit N_PARAM");

const AlgorithmDescriptor DrumSeqGate::descriptor = {
    ALGO_DRUM_SEQ_GATE, "DrumSeqGate", 2, 1, DRUM_SEQ_LANES, DrumSeqGate::PARAM_COUNT,
    IN, GATE_OUT, sizeof(DrumSeqGate), false, construct_node<DrumSeqGate> };

const AlgorithmDescriptor DrumSeqMidi::descriptor = {
    ALGO_DRUM_SEQ_MIDI, "DrumSeqMidi", 2, 1, 1, DrumSeqMidi::PARAM_COUNT,
    IN, NOTE_OUT, sizeof(DrumSeqMidi), false, construct_node<DrumSeqMidi> };

// General MIDI, so an unconfigured lane lands on something a drum machine
// answers to: kick, snare, closed hat, open hat, low tom, mid tom, crash, ride.
static const uint8_t GM_DEFAULT_NOTE[DRUM_SEQ_LANES] = {36, 38, 42, 46, 41, 45, 49, 51};

// The grid ------------------------------------------------------------------

DrumSequencer::DrumSequencer(const NodeConfig& config) :
    lanes(), rng(entropy::seed()), chance(),
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1])
{
    for (uint8_t l = 0; l < LANES; l++) chance[l] = 100;
    // Lane lengths are set by the subclass, which knows where they live.
    for (uint8_t l = 0; l < LANES; l++) lanes[l].configure(config.params[P_LENGTH], config.params[P_DIRECTION], DEFAULT_LENGTH);
}

void DrumSequencer::process(BusManager& bus, uint32_t now_us){
    if (reset_in.rising(bus)) for (uint8_t l = 0; l < LANES; l++) lanes[l].reset();
    if (advance_in.rising(bus)){
        for (uint8_t l = 0; l < LANES; l++){
            const uint8_t step = lanes[l].advance(rng);
            if (hit(l, step) && rng.chance(chance[l])) fire(bus, l, now_us);
        }
    }
    run(bus, now_us);
}

// To gates ------------------------------------------------------------------

DrumSeqGate::DrumSeqGate(const NodeConfig& config) :
    DrumSequencer(config), out(), bits(), pulse()
{
    for (uint8_t l = 0; l < LANES; l++){
        const uint8_t* lane = &config.params[LANE_BASE + l * LANE_STRIDE];
        out[l] = config.out_bus[l];
        bits[l] = (uint32_t)lane[0] | ((uint32_t)lane[1] << 8) | ((uint32_t)lane[2] << 16) | ((uint32_t)lane[3] << 24);
        if (lane[4]) lanes[l].configure(lane[4], config.params[P_DIRECTION], DEFAULT_LENGTH);
        chance[l] = step_probability(lane[5]);
        if (config.params[P_GATE]) pulse[l].set_width_us((uint32_t)config.params[P_GATE] * 1000u);
    }
}

void DrumSeqGate::fire(BusManager&, uint8_t lane, uint32_t now_us){
    pulse[lane].fire(now_us);
}

void DrumSeqGate::run(BusManager& bus, uint32_t now_us){
    for (uint8_t l = 0; l < LANES; l++){
        if (pulse[l].level(now_us)) bus.gate_write(out[l], true);   // NO_BUS is ignored by the bus
    }
}

// To MIDI -------------------------------------------------------------------

DrumSeqMidi::DrumSeqMidi(const NodeConfig& config) :
    DrumSequencer(config),
    out(config.out_bus[0]),
    gate_us((uint32_t)(config.params[P_GATE] ? config.params[P_GATE] : DEFAULT_GATE_MS) * 1000u),
    note(), channel(), velocity(), off_at_us(), playing(), sounding()
{
    for (uint8_t l = 0; l < LANES; l++){
        const uint8_t* lane = &config.params[LANE_BASE + l * LANE_STRIDE];
        note[l] = lane[0] ? (uint8_t)(lane[0] & 0x7F) : GM_DEFAULT_NOTE[l];
        channel[l] = lane[1] ? (uint8_t)(((lane[1] - 1u) % 16u) + 1u) : DEFAULT_CHANNEL;
        if (lane[2]) lanes[l].configure(lane[2], config.params[P_DIRECTION], DEFAULT_LENGTH);
        chance[l] = step_probability(lane[3]);
        for (uint8_t s = 0; s < MAX_SEQUENCE_LEN; s++) velocity[l][s] = config.params[VELOCITY_BASE + l * MAX_SEQUENCE_LEN + s] & 0x7F;
        off_at_us[l] = 0;
        playing[l] = false;
    }
}

void DrumSeqMidi::release(BusManager& bus, uint8_t lane){
    if (!playing[lane]) return;
    sounding.release(bus, out, lane);
    playing[lane] = false;
}

void DrumSeqMidi::fire(BusManager& bus, uint8_t lane, uint32_t now_us){
    release(bus, lane);                                     // a retrigger ends the last hit first
    const uint8_t vel = velocity[lane][lanes[lane].position()];
    // The ledger keys on the lane, so the release finds the note number that
    // was sent, whatever the lane is configured as by then.
    if (sounding.emit(bus, out, lane, note[lane], vel, channel[lane])){
        playing[lane] = true;
        off_at_us[lane] = now_us + gate_us;
    }
}

void DrumSeqMidi::run(BusManager& bus, uint32_t now_us){
    for (uint8_t l = 0; l < LANES; l++){
        if (playing[l] && (uint32_t)(now_us - off_at_us[l]) < 0x80000000u) release(bus, l);
    }
}

void DrumSeqMidi::silence(BusManager& bus){
    for (uint8_t l = 0; l < LANES; l++) release(bus, l);
    sounding.release_all(bus, out);
}
