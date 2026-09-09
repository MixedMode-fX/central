#include "algorithm/sequencer/note_sequencer.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/scale.h"

static const Domain IN[3] = {Domain::Gate, Domain::Gate, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor NoteSequencer::descriptor = {
    ALGO_NOTE_SEQ, "NoteSequencer", 3, 1, 1, NoteSequencerBase::param_count(1),
    IN, OUT, sizeof(NoteSequencer), false, construct_node<NoteSequencer> };

const AlgorithmDescriptor PolySequencer::descriptor = {
    ALGO_POLY_SEQ, "PolySequencer", 3, 1, 1, NoteSequencerBase::param_count(NOTE_SEQ_VOICES),
    IN, OUT, sizeof(PolySequencer), false, construct_node<PolySequencer> };

static_assert(NoteSequencerBase::param_count(NOTE_SEQ_VOICES) <= N_PARAM, "PolySequencer's steps do not fit N_PARAM");

NoteSequencerBase::NoteSequencerBase(const NodeConfig& config, uint8_t voices_per_step) :
    advance_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    root_in(config.in_bus[2]),
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
    if (reset_in.rising(bus)) engine.reset();

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
