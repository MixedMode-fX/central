#include "algorithm/sequencer/note_sequencer.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/scale.h"

// advance, reset, root, record, record-enable (#22).
static const Domain IN[5] = {Domain::Gate, Domain::Gate, Domain::Note, Domain::Note, Domain::Gate};
static_assert(MAX_IN >= 5, "the note sequencers need five inlets");
static const Domain OUT[1] = {Domain::Note};

// Parameter descriptors (#20): a 16-byte header, then the steps as one
// repeating group whose stride is the step size. The poly variant's 336
// parameters are two groups and 26 descriptors this way, instead of 336
// hand-written entries that would drift the moment a voice was added.
const ParamDescriptor NoteSequencerBase::HEADER[16] = {
    {"length",     1, MAX_SEQUENCE_LEN,               8,  PARAM_NUMBER,  nullptr},
    {"direction",  0, StepEngine::SEQ_DIRECTIONS - 1, 0,  PARAM_ENUM,    PARAM_DIRECTION_NAMES},
    {"gate",       0, 100,                            0,  PARAM_PERCENT, nullptr},
    {"scale low",  0, 255,                            0,  PARAM_BITFIELD, nullptr},
    {"scale high", 0, 15,                             0,  PARAM_BITFIELD, nullptr},
    {"root",       0, 127,                            60, PARAM_PITCH,   nullptr},
    {"vel scale",  1, 255,                            100, PARAM_PERCENT, nullptr},
    {"vel offset", 0, 255,                            0,  PARAM_SIGNED,  nullptr},
    {"channel",    1, 16,                             1,  PARAM_CHANNEL, nullptr},
    {"accent",     0, 127,                            30, PARAM_NUMBER,  nullptr},
    {"stall",      0, 255,                            4,  PARAM_NUMBER,  nullptr},
    {"rest key",   0, 127,                            0,  PARAM_PITCH,   nullptr},
    {"tie key",    0, 127,                            1,  PARAM_PITCH,   nullptr},
    {"rec velocity", 0, 127,                          0,  PARAM_NUMBER,  nullptr},
    {"reserved",   0, 0,                              0,  PARAM_NUMBER,  nullptr},
    {"reserved",   0, 0,                              0,  PARAM_NUMBER,  nullptr},
};

// A step is described as one group of MAX_SEQUENCE_LEN repeats whose field
// array is the whole step: the voice pairs then the two tail bytes. The tail
// does not repeat with the pairs, so the step - not the pair - is the unit
// that recurs.
static const ParamDescriptor MONO_STEP[NoteSequencerBase::stride(1)] = {
    {"degree",   0, 255, 0,   PARAM_SIGNED,   nullptr},
    {"velocity", 0, 127, 0,   PARAM_NUMBER,   nullptr},
    {"length/flags", 0, 255, 0, PARAM_BITFIELD, nullptr},
    {"probability",  0, 100, 100, PARAM_PERCENT, nullptr},
};

static const ParamDescriptor POLY_STEP[NoteSequencerBase::stride(NOTE_SEQ_VOICES)] = {
    {"degree 1",   0, 255, 0, PARAM_SIGNED, nullptr},
    {"velocity 1", 0, 127, 0, PARAM_NUMBER, nullptr},
    {"degree 2",   0, 255, 0, PARAM_SIGNED, nullptr},
    {"velocity 2", 0, 127, 0, PARAM_NUMBER, nullptr},
    {"degree 3",   0, 255, 0, PARAM_SIGNED, nullptr},
    {"velocity 3", 0, 127, 0, PARAM_NUMBER, nullptr},
    {"degree 4",   0, 255, 0, PARAM_SIGNED, nullptr},
    {"velocity 4", 0, 127, 0, PARAM_NUMBER, nullptr},
    {"length/flags", 0, 255, 0, PARAM_BITFIELD, nullptr},
    {"probability",  0, 100, 100, PARAM_PERCENT, nullptr},
};

static_assert(NOTE_SEQ_VOICES == 4, "POLY_STEP lists one degree/velocity pair per voice");

static const ParamGroup MONO_GROUPS[2] = {
    {0, 1, 16, NoteSequencerBase::HEADER},
    {NoteSequencerBase::STEP_BASE, MAX_SEQUENCE_LEN, NoteSequencerBase::stride(1), MONO_STEP},
};

static const ParamGroup POLY_GROUPS[2] = {
    {0, 1, 16, NoteSequencerBase::HEADER},
    {NoteSequencerBase::STEP_BASE, MAX_SEQUENCE_LEN, NoteSequencerBase::stride(NOTE_SEQ_VOICES), POLY_STEP},
};

const AlgorithmDescriptor NoteSequencer::descriptor = {
    ALGO_NOTE_SEQ, "NoteSequencer", 5, 1, 1, NoteSequencerBase::param_count(1),
    IN, OUT, sizeof(NoteSequencer), false, construct_node<NoteSequencer>,
    MONO_GROUPS, 2 };

const AlgorithmDescriptor PolySequencer::descriptor = {
    ALGO_POLY_SEQ, "PolySequencer", 5, 1, 1, NoteSequencerBase::param_count(NOTE_SEQ_VOICES),
    IN, OUT, sizeof(PolySequencer), false, construct_node<PolySequencer>,
    POLY_GROUPS, 2 };

static_assert(NoteSequencerBase::param_count(NOTE_SEQ_VOICES) <= N_PARAM, "PolySequencer's steps do not fit N_PARAM");

NoteSequencerBase::NoteSequencerBase(const NodeConfig& config, uint8_t voices_per_step) :
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    root_in(config.in_bus[2]),
    rec_in(config.in_bus[3]),
    rec_enable_in(config.in_bus[4]),
    rec_enable_bus(config.in_bus[4]),
    out(config.out_bus[0]),
    n_voices(voices_per_step == 0 ? 1 : (voices_per_step > MAX_VOICES ? MAX_VOICES : voices_per_step)),
    scale_mask((uint16_t)(config.params[P_SCALE_LO] | ((uint16_t)(config.params[P_SCALE_HI] & 0x0F) << 8))),
    root(config.params[P_ROOT] ? (uint8_t)(config.params[P_ROOT] & 0x7F) : DEFAULT_ROOT),
    gate_pct(config.params[P_GATE] > 100 ? 100 : config.params[P_GATE]),
    vel_scale(config.params[P_VEL_SCALE] ? config.params[P_VEL_SCALE] : 100),
    vel_offset((int8_t)config.params[P_VEL_OFFSET]),
    channel(config.params[P_CHANNEL] ? (uint8_t)(((config.params[P_CHANNEL] - 1u) % 16u) + 1u) : 1),
    accent(config.params[P_ACCENT] ? config.params[P_ACCENT] : DEFAULT_ACCENT),
    stall_periods(config.params[P_STALL] ? config.params[P_STALL] : DEFAULT_STALL),
    rest_key(config.params[P_REST_KEY]),
    tie_key(config.params[P_TIE_KEY] ? config.params[P_TIE_KEY] : DEFAULT_TIE_KEY),
    rec_velocity(config.params[P_REC_VELOCITY]),
    rec_cursor(0), snap_count(0),
    last_edge_us(0), period(0), have_edge(false), have_period(false),
    engine(), rng(entropy::seed()), sounding(), voice(), steps()
{
    engine.configure(config.params[P_LENGTH], config.params[P_DIRECTION], DEFAULT_LENGTH);
    const uint16_t bytes = (uint16_t)(MAX_SEQUENCE_LEN * stride(n_voices));
    for (uint16_t i = 0; i < bytes; i++) steps[i] = config.params[STEP_BASE + i];
    for (uint8_t v = 0; v < MAX_VOICES; v++) voice[v] = Voice{0, 0, false, false};
}

// Step data ----------------------------------------------------------------

int8_t NoteSequencerBase::degree(uint8_t step, uint8_t v) const {
    if (step >= MAX_SEQUENCE_LEN || v >= n_voices) return 0;
    return (int8_t)step_bytes(step)[v * 2u];
}

uint8_t NoteSequencerBase::velocity(uint8_t step, uint8_t v) const {
    if (step >= MAX_SEQUENCE_LEN || v >= n_voices) return 0;
    return step_bytes(step)[v * 2u + 1u] & 0x7F;
}

uint8_t NoteSequencerBase::flags(uint8_t step) const {
    if (step >= MAX_SEQUENCE_LEN) return 0;
    return step_bytes(step)[n_voices * 2u];
}

uint8_t NoteSequencerBase::probability(uint8_t step) const {
    if (step >= MAX_SEQUENCE_LEN) return 100;
    return step_probability(step_bytes(step)[n_voices * 2u + 1u]);
}

// Parameters ----------------------------------------------------------------

bool NoteSequencerBase::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_LENGTH:
            if (value > MAX_SEQUENCE_LEN) return false;
            engine.set_length(value, DEFAULT_LENGTH);
            return true;
        case P_DIRECTION:
            if (value >= StepEngine::SEQ_DIRECTIONS) return false;
            engine.set_direction(value);
            return true;
        case P_GATE:
            if (value > 100) return false;
            gate_pct = value;
            return true;
        case P_SCALE_LO:
            scale_mask = (uint16_t)((scale_mask & 0x0F00u) | value);
            return true;
        case P_SCALE_HI:
            if (value > 0x0F) return false;
            scale_mask = (uint16_t)((scale_mask & 0x00FFu) | ((uint16_t)value << 8));
            return true;
        case P_ROOT:
            // Sounding notes are released from the ledger at the pitch they
            // were sent at, so the root can move under a held note.
            root = value ? (uint8_t)(value & 0x7F) : DEFAULT_ROOT;
            return true;
        case P_VEL_SCALE:  vel_scale = value ? value : 100; return true;
        case P_VEL_OFFSET: vel_offset = (int8_t)value; return true;
        case P_CHANNEL:
            if (value > 16) return false;
            channel = value ? value : 1;
            return true;
        case P_ACCENT:
            if (value > 127) return false;
            accent = value ? value : DEFAULT_ACCENT;
            return true;
        case P_STALL:
            stall_periods = value ? value : DEFAULT_STALL;
            return true;
        case P_REST_KEY:
            if (value > 127) return false;
            rest_key = value;
            return true;
        case P_TIE_KEY:
            if (value > 127) return false;
            tie_key = value ? value : DEFAULT_TIE_KEY;
            return true;
        case P_REC_VELOCITY:
            if (value > 127) return false;
            rec_velocity = value;
            return true;
        default:
            break;
    }
    if (index >= STEP_BASE && index < param_count(n_voices)){
        steps[index - STEP_BASE] = value;
        return true;
    }
    return false;                     // params[11..15] are reserved
}

uint8_t NoteSequencerBase::get_param(uint16_t index) const {
    switch (index){
        case P_LENGTH:     return engine.length();
        case P_DIRECTION:  return engine.direction();
        case P_GATE:       return gate_pct;
        case P_SCALE_LO:   return (uint8_t)(scale_mask & 0xFFu);
        case P_SCALE_HI:   return (uint8_t)((scale_mask >> 8) & 0x0Fu);
        case P_ROOT:       return root;
        case P_VEL_SCALE:  return vel_scale;
        case P_VEL_OFFSET: return (uint8_t)vel_offset;
        case P_CHANNEL:    return channel;
        case P_ACCENT:     return accent;
        case P_STALL:      return stall_periods;
        case P_REST_KEY:   return rest_key;
        case P_TIE_KEY:    return tie_key;
        case P_REC_VELOCITY: return rec_velocity;
        default:           break;
    }
    if (index >= STEP_BASE && index < param_count(n_voices)) return steps[index - STEP_BASE];
    return 0;
}

void NoteSequencerBase::set_step(uint8_t step, uint8_t v, int8_t deg, uint8_t vel){
    if (step >= MAX_SEQUENCE_LEN || v >= n_voices) return;
    uint8_t* b = &steps[(uint16_t)step * stride(n_voices)];
    b[v * 2u] = (uint8_t)deg;
    b[v * 2u + 1u] = vel & 0x7F;
}

uint8_t NoteSequencerBase::pitch(uint8_t step, uint8_t v) const {
    if (velocity(step, v) == 0) return NO_PITCH;
    const int16_t p = (int16_t)root + scale_degree_to_semitone(degree(step, v), scale_mask);
    if (p < 0 || p > 127) return NO_PITCH;                // skipped, never wrapped
    return (uint8_t)p;
}

uint8_t NoteSequencerBase::sent_velocity(uint8_t step, uint8_t v) const {
    const uint8_t stored = velocity(step, v);
    if (stored == 0) return 0;
    int16_t value = stored;
    if (flags(step) & FLAG_ACCENT) value = (int16_t)(value + accent);
    value = (int16_t)((value * vel_scale) / 100 + vel_offset);
    if (value < 1) value = 1;
    if (value > 127) value = 127;
    return (uint8_t)value;
}

// Voices -------------------------------------------------------------------

bool NoteSequencerBase::any_sounding() const {
    for (uint8_t v = 0; v < n_voices; v++) if (voice[v].sounding) return true;
    return false;
}

void NoteSequencerBase::release_voice(BusManager& bus, uint8_t v){
    if (!voice[v].sounding) return;
    sounding.release(bus, out, v);                          // the pitch it actually sent
    voice[v] = Voice{0, 0, false, false};
}

void NoteSequencerBase::release_all(BusManager& bus){
    for (uint8_t v = 0; v < MAX_VOICES; v++) release_voice(bus, v);
    sounding.release_all(bus, out);                         // belt and braces: the ledger is the truth
}

// Step-record ---------------------------------------------------------------

void NoteSequencerBase::advance_record_cursor(){
    rec_cursor = (uint8_t)((rec_cursor + 1u) % (engine.length() ? engine.length() : 1u));
}

// A keyboard sends a pitch; a step stores a degree. A note outside the current
// scale is snapped to the nearest tone in it - the same rule Quantise
// follows - rather than refused, because a step-record that silently dropped
// a note would be worse than one that put it a semitone away. Snaps are
// counted so a user can see it happening.
void NoteSequencerBase::record_note(uint8_t pitch, uint8_t velocity){
    const uint16_t mask = scale_mask ? scale_mask : (uint16_t)0x0FFF;
    const uint8_t snapped_pitch = scale_quantise(pitch, (uint8_t)(root % 12u), mask);
    if (snapped_pitch != pitch) snap_count++;

    // Semitones from the root, then which degree of the scale that is.
    const int16_t semitones = (int16_t)snapped_pitch - (int16_t)root;
    const int16_t degree = semitone_to_scale_degree(semitones, mask);

    const uint8_t vel = rec_velocity ? rec_velocity : (velocity ? velocity : 100);
    // Voice 0: step-record is monophonic entry even on the poly sequencer.
    // Chord entry wants held-note grouping and belongs with real-time record.
    set_step(rec_cursor, 0, (int8_t)degree, vel);
    uint8_t* b = step_bytes(rec_cursor);
    b[n_voices * 2u] = (uint8_t)(b[n_voices * 2u] & ~(LENGTH_MASK | FLAG_REST | FLAG_TIE));
    b[n_voices * 2u] = (uint8_t)(b[n_voices * 2u] | 1u);        // one edge long
    advance_record_cursor();
}

void NoteSequencerBase::write_rest(){
    uint8_t* b = step_bytes(rec_cursor);
    b[n_voices * 2u] = (uint8_t)((b[n_voices * 2u] & ~FLAG_TIE) | FLAG_REST);
    advance_record_cursor();
}

void NoteSequencerBase::write_tie(){
    uint8_t* b = step_bytes(rec_cursor);
    b[n_voices * 2u] = (uint8_t)((b[n_voices * 2u] & ~FLAG_REST) | FLAG_TIE);
    advance_record_cursor();
}

void NoteSequencerBase::silence(BusManager& bus){
    release_all(bus);
}

// The last edge-unit of a note is shortened to gate_pct of a step, if a
// step's duration has been measured. Without a measurement the note stays
// exact and ends on its edge.
void NoteSequencerBase::time_last_unit(Voice& v, uint32_t now_us) const {
    if (gate_pct == 0 || !have_period) return;
    v.timed = true;
    v.release_at_us = now_us + (uint32_t)(((uint64_t)period * gate_pct) / 100u);
}

void NoteSequencerBase::play_step(BusManager& bus, uint8_t step, uint32_t now_us){
    const uint8_t f = flags(step);
    const uint8_t len = step_length(step);
    const bool play = !(f & FLAG_REST) && rng.chance(probability(step));
    const bool tie = play && (f & FLAG_TIE) && any_sounding();

    // What is already sounding: extended by a tie, otherwise one edge closer
    // to its release.
    for (uint8_t v = 0; v < n_voices; v++){
        Voice& s = voice[v];
        if (!s.sounding) continue;
        if (tie){
            s.edges_left = (uint8_t)(s.edges_left + len);
            s.timed = false;
        } else {
            s.edges_left--;
            if (s.edges_left == 0){ release_voice(bus, v); continue; }
        }
        if (s.edges_left == 1) time_last_unit(s, now_us);
    }
    if (!play || tie) return;

    for (uint8_t v = 0; v < n_voices; v++){
        const uint8_t vel = sent_velocity(step, v);
        if (vel == 0) continue;
        if (voice[v].sounding) release_voice(bus, v);       // one note per voice
        const uint8_t p = pitch(step, v);
        if (p == NO_PITCH) continue;
        // The ledger keys on the voice, so the release finds the note that
        // was sent whatever the root or scale is by then.
        if (!sounding.emit(bus, out, v, p, vel, channel)) continue;
        voice[v] = Voice{len, 0, true, false};
        if (len == 1) time_last_unit(voice[v], now_us);
    }
}

// The pass ------------------------------------------------------------------

void NoteSequencerBase::process(BusManager& bus, uint32_t now_us){
    // The root first, so a root and an edge arriving in the same pass agree.
    if (root_in != NO_BUS){
        const uint8_t n = bus.note_count(root_in);
        for (uint8_t i = 0; i < n; i++){
            const MidiEvent e = bus.note_read(root_in, i);
            if (is_note_on(e)) root = e.data1 & 0x7F;
        }
    }
    if (reset_in.rising(bus)){
        engine.reset();
        rec_cursor = 0;             // reset means the same thing to the writer
    }

    // Step-record (#22). Before the advance, so a note played in the same
    // pass as an edge is written to the step it was aimed at rather than the
    // one after it.
    if (rec_in != NO_BUS){
        const bool armed = (rec_enable_bus == NO_BUS) || bus.gate_read(rec_enable_bus);
        rec_enable_in.rising(bus);           // keep the edge detector in step
        if (armed){
            const uint8_t n = bus.note_count(rec_in);
            for (uint8_t i = 0; i < n; i++){
                const MidiEvent e = bus.note_read(rec_in, i);
                if (!is_note_on(e)) continue;
                if (e.data1 == rest_key){ write_rest(); continue; }
                if (e.data1 == tie_key){ write_tie(); continue; }
                record_note(e.data1, e.data2);
            }
        }
    }

    // Estimated releases that have come due.
    for (uint8_t v = 0; v < n_voices; v++){
        const Voice& s = voice[v];
        if (s.sounding && s.timed && (uint32_t)(now_us - s.release_at_us) < 0x80000000u) release_voice(bus, v);
    }
    // A clock that stopped: after `stall_periods` periods without an edge,
    // whatever is sounding is released rather than held for ever.
    if (have_edge && stall_periods != STALL_NEVER && any_sounding()){
        const uint32_t per = have_period ? period : STALL_UNKNOWN_PERIOD_US;
        if ((uint32_t)(now_us - last_edge_us) >= per * stall_periods) release_all(bus);
    }

    if (!advance_in.rising(bus)) return;
    if (have_edge){
        const uint32_t measured = now_us - last_edge_us;
        if (measured > 0){ period = measured; have_period = true; }
    }
    have_edge = true;
    last_edge_us = now_us;
    play_step(bus, engine.advance(rng), now_us);
}
