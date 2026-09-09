#include "algorithm/sequencer/drum_sequencer.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain GATE_OUT[DRUM_SEQ_LANES] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };
static const Domain NOTE_OUT[1] = {Domain::Note};

static_assert(DRUM_SEQ_LANES == 8, "GATE_OUT lists one domain per lane");
static_assert(DRUM_SEQ_LANES <= MAX_OUT, "DrumSeqGate has one outlet per lane");
static_assert(DrumSeqMidi::PARAM_COUNT <= N_PARAM, "DrumSeqMidi's grid does not fit N_PARAM");

// Parameter descriptors (#20): a 16-byte header, then the lanes as one
// repeating group of LANE_STRIDE bytes. The MIDI variant adds a third group
// for the velocity grid, which is one descriptor covering 256 parameters.
const ParamDescriptor DrumSequencer::HEADER[16] = {
    {"length",    1, MAX_SEQUENCE_LEN,               16, PARAM_NUMBER, nullptr},
    {"direction", 0, StepEngine::SEQ_DIRECTIONS - 1, 0,  PARAM_ENUM,   PARAM_DIRECTION_NAMES},
    {"gate",      0, 255,                            0,  PARAM_MILLIS, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",  0, 0, 0, PARAM_NUMBER, nullptr},
};

static const ParamDescriptor GATE_LANE[DrumSequencer::LANE_STRIDE] = {
    {"steps 1-8",   0, 255, 0, PARAM_BITFIELD, nullptr},
    {"steps 9-16",  0, 255, 0, PARAM_BITFIELD, nullptr},
    {"steps 17-24", 0, 255, 0, PARAM_BITFIELD, nullptr},
    {"steps 25-32", 0, 255, 0, PARAM_BITFIELD, nullptr},
    {"length",      0, MAX_SEQUENCE_LEN, 0, PARAM_NUMBER,  nullptr},
    {"probability", 0, 100, 100, PARAM_PERCENT, nullptr},
    {"reserved",    0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",    0, 0, 0, PARAM_NUMBER, nullptr},
};

static const ParamDescriptor MIDI_LANE[DrumSequencer::LANE_STRIDE] = {
    {"note",        0, 127, 0, PARAM_PITCH,   nullptr},
    {"channel",     0, 16,  DrumSeqMidi::DEFAULT_CHANNEL, PARAM_CHANNEL, nullptr},
    {"length",      0, MAX_SEQUENCE_LEN, 0, PARAM_NUMBER, nullptr},
    {"probability", 0, 100, 100, PARAM_PERCENT, nullptr},
    {"reserved",    0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
    {"reserved",    0, 0, 0, PARAM_NUMBER, nullptr}, {"reserved", 0, 0, 0, PARAM_NUMBER, nullptr},
};

static const ParamDescriptor MIDI_VELOCITY[1] = {
    {"velocity", 0, 127, 0, PARAM_NUMBER, nullptr},
};

static const ParamGroup GATE_GROUPS[2] = {
    {0, 1, 16, DrumSequencer::HEADER},
    {DrumSequencer::LANE_BASE, DRUM_SEQ_LANES, DrumSequencer::LANE_STRIDE, GATE_LANE},
};

static const ParamGroup MIDI_GROUPS[3] = {
    {0, 1, 16, DrumSequencer::HEADER},
    {DrumSequencer::LANE_BASE, DRUM_SEQ_LANES, DrumSequencer::LANE_STRIDE, MIDI_LANE},
    {DrumSeqMidi::VELOCITY_BASE, DRUM_SEQ_LANES * MAX_SEQUENCE_LEN, 1, MIDI_VELOCITY},
};

// One outlet per lane on the gate variant, so the names are what tells a user
// which jack a lane reaches - "out 3" would not.
static const char* const DRUM_IN_NAMES[2] = {"advance", "reset"};
static const char* const DRUM_GATE_OUT_NAMES[DRUM_SEQ_LANES] = {
    "lane 1", "lane 2", "lane 3", "lane 4", "lane 5", "lane 6", "lane 7", "lane 8"};
static const char* const DRUM_NOTE_OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor DrumSeqGate::descriptor = {
    ALGO_DRUM_SEQ_GATE, "DrumSeqGate", 2, 1, DRUM_SEQ_LANES, DrumSeqGate::PARAM_COUNT,
    IN, GATE_OUT, sizeof(DrumSeqGate), false, construct_node<DrumSeqGate>,
    GATE_GROUPS, 2, DRUM_IN_NAMES, DRUM_GATE_OUT_NAMES,
    "Eight gate lanes on one grid, each with its own length. One outlet per lane." };

const AlgorithmDescriptor DrumSeqMidi::descriptor = {
    ALGO_DRUM_SEQ_MIDI, "DrumSeqMidi", 2, 1, 1, DrumSeqMidi::PARAM_COUNT,
    IN, NOTE_OUT, sizeof(DrumSeqMidi), false, construct_node<DrumSeqMidi>,
    MIDI_GROUPS, 3, DRUM_IN_NAMES, DRUM_NOTE_OUT_NAMES,
    "Eight drum lanes as MIDI: a note and channel per lane, a velocity per cell." };

// General MIDI, so an unconfigured lane lands on something a drum machine
// answers to: kick, snare, closed hat, open hat, low tom, mid tom, crash, ride.
static const uint8_t GM_DEFAULT_NOTE[DRUM_SEQ_LANES] = {36, 38, 42, 46, 41, 45, 49, 51};

// The grid ------------------------------------------------------------------

DrumSequencer::DrumSequencer(const NodeConfig& config) :
    header_length(config.params[P_LENGTH]),
    header_direction(config.params[P_DIRECTION]),
    gate_param(config.params[P_GATE]),
    lanes(), rng(entropy::seed()), chance(), lane_len(),
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1])
{
    for (uint8_t l = 0; l < LANES; l++) chance[l] = 100;
    // Lane lengths are set by the subclass, which knows where they live.
    for (uint8_t l = 0; l < LANES; l++) lanes[l].configure(header_length, header_direction, DEFAULT_LENGTH);
}

bool DrumSequencer::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_LENGTH:
            if (value > MAX_SEQUENCE_LEN) return false;
            header_length = value;
            // Only the lanes that follow the header move; a lane with its own
            // length keeps it. Cursors are untouched and clamp on the next
            // advance, so a polyrhythm does not stumble.
            for (uint8_t l = 0; l < LANES; l++){
                if (lane_len[l] == 0) lanes[l].set_length(value, DEFAULT_LENGTH);
            }
            return true;
        case P_DIRECTION:
            if (value >= StepEngine::SEQ_DIRECTIONS) return false;
            header_direction = value;
            for (uint8_t l = 0; l < LANES; l++) lanes[l].set_direction(value);
            return true;
        case P_GATE:
            gate_param = value;
            return true;      // the subclass turns it into a width or a length
        default:
            return false;     // params[3..15] are reserved
    }
}

uint8_t DrumSequencer::get_param(uint16_t index) const {
    switch (index){
        case P_LENGTH:    return header_length ? header_length : DEFAULT_LENGTH;
        case P_DIRECTION: return header_direction;
        case P_GATE:      return gate_param;
        default:          return 0;
    }
}

// Where a lane's byte lives, or LANES when the index is not lane data.
static inline uint8_t lane_of(uint16_t index, uint8_t& field){
    if (index < DrumSequencer::LANE_BASE) return DrumSequencer::LANES;
    const uint16_t offset = (uint16_t)(index - DrumSequencer::LANE_BASE);
    const uint16_t lane = (uint16_t)(offset / DrumSequencer::LANE_STRIDE);
    if (lane >= DrumSequencer::LANES) return DrumSequencer::LANES;
    field = (uint8_t)(offset % DrumSequencer::LANE_STRIDE);
    return (uint8_t)lane;
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
    DrumSequencer(config), out(), bits(), width_param(config.params[P_GATE]), pulse()
{
    for (uint8_t l = 0; l < LANES; l++){
        const uint8_t* lane = &config.params[LANE_BASE + l * LANE_STRIDE];
        out[l] = config.out_bus[l];
        bits[l] = (uint32_t)lane[0] | ((uint32_t)lane[1] << 8) | ((uint32_t)lane[2] << 16) | ((uint32_t)lane[3] << 24);
        lane_len[l] = lane[4];
        if (lane[4]) lanes[l].configure(lane[4], config.params[P_DIRECTION], DEFAULT_LENGTH);
        chance[l] = step_probability(lane[5]);
        if (config.params[P_GATE]) pulse[l].set_width_us((uint32_t)config.params[P_GATE] * 1000u);
    }
}

bool DrumSeqGate::set_param(uint16_t index, uint8_t value){
    if (index == P_GATE){
        if (!DrumSequencer::set_param(index, value)) return false;
        width_param = value;
        const uint32_t us = value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US;
        for (uint8_t l = 0; l < LANES; l++) pulse[l].set_width_us(us);
        return true;
    }
    uint8_t field = 0;
    const uint8_t lane = lane_of(index, field);
    if (lane >= LANES) return DrumSequencer::set_param(index, value);
    switch (field){
        case 0: case 1: case 2: case 3: {
            const uint8_t shift = (uint8_t)(8u * field);
            bits[lane] = (bits[lane] & ~((uint32_t)0xFFu << shift)) | ((uint32_t)value << shift);
            return true;
        }
        case 4:
            if (value > MAX_SEQUENCE_LEN) return false;
            lane_len[lane] = value;
            lanes[lane].set_length(value ? value : header_length, DEFAULT_LENGTH);
            return true;
        case 5:
            if (value > 100) return false;
            chance[lane] = step_probability(value);
            return true;
        default:
            return false;                       // lane bytes 6..7 are reserved
    }
}

uint8_t DrumSeqGate::get_param(uint16_t index) const {
    if (index == P_GATE) return width_param;
    uint8_t field = 0;
    const uint8_t lane = lane_of(index, field);
    if (lane >= LANES) return DrumSequencer::get_param(index);
    switch (field){
        case 0: case 1: case 2: case 3: return (uint8_t)(bits[lane] >> (8u * field));
        case 4: return lane_len[lane];
        case 5: return chance[lane];
        default: return 0;
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
        lane_len[l] = lane[2];
        if (lane[2]) lanes[l].configure(lane[2], config.params[P_DIRECTION], DEFAULT_LENGTH);
        chance[l] = step_probability(lane[3]);
        for (uint8_t s = 0; s < MAX_SEQUENCE_LEN; s++) velocity[l][s] = config.params[VELOCITY_BASE + l * MAX_SEQUENCE_LEN + s] & 0x7F;
        off_at_us[l] = 0;
        playing[l] = false;
    }
}

// Every note-on is released from the ledger at the pitch it was sent at, so
// a lane's note number, channel, pattern or length can all move under a
// sounding hit without stranding it.
bool DrumSeqMidi::set_param(uint16_t index, uint8_t value){
    if (index == P_GATE){
        if (!DrumSequencer::set_param(index, value)) return false;
        gate_us = (uint32_t)(value ? value : DEFAULT_GATE_MS) * 1000u;
        return true;
    }
    if (index >= VELOCITY_BASE && index < PARAM_COUNT){
        if (value > 127) return false;
        const uint16_t cell = (uint16_t)(index - VELOCITY_BASE);
        velocity[cell / MAX_SEQUENCE_LEN][cell % MAX_SEQUENCE_LEN] = value;
        return true;
    }
    uint8_t field = 0;
    const uint8_t lane = lane_of(index, field);
    if (lane >= LANES) return DrumSequencer::set_param(index, value);
    switch (field){
        case 0:
            if (value > 127) return false;
            note[lane] = value ? value : GM_DEFAULT_NOTE[lane];
            return true;
        case 1:
            if (value > 16) return false;
            channel[lane] = value ? value : DEFAULT_CHANNEL;
            return true;
        case 2:
            if (value > MAX_SEQUENCE_LEN) return false;
            lane_len[lane] = value;
            lanes[lane].set_length(value ? value : header_length, DEFAULT_LENGTH);
            return true;
        case 3:
            if (value > 100) return false;
            chance[lane] = step_probability(value);
            return true;
        default:
            return false;                       // lane bytes 4..7 are reserved
    }
}

uint8_t DrumSeqMidi::get_param(uint16_t index) const {
    if (index == P_GATE) return DrumSequencer::get_param(index);
    if (index >= VELOCITY_BASE && index < PARAM_COUNT){
        const uint16_t cell = (uint16_t)(index - VELOCITY_BASE);
        return velocity[cell / MAX_SEQUENCE_LEN][cell % MAX_SEQUENCE_LEN];
    }
    uint8_t field = 0;
    const uint8_t lane = lane_of(index, field);
    if (lane >= LANES) return DrumSequencer::get_param(index);
    switch (field){
        case 0: return note[lane];
        case 1: return channel[lane];
        case 2: return lane_len[lane];
        case 3: return chance[lane];
        default: return 0;
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
