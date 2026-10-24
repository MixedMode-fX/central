#include "algorithm/midi/rps10.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "util/tone.h"
#include "midi/global_key.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::CV};

static const char* const RELEASE_NAMES[3] = {"hold", "unison", "off"};

static const ParamDescriptor PARAMS[4] = {
    {"priority", Rps10::PRIORITY_LOWEST, Rps10::PRIORITIES, Rps10::PRIORITY_LATEST,
     PARAM_ENUM, PARAM_PRIORITY_NAMES},
    {"octave",   0, KEY_MAX_OCTAVE, 0, PARAM_ENUM, PARAM_OCTAVE_NAMES},
    {"bend",     0, Rps10::MAX_BEND, Rps10::DEFAULT_BEND, PARAM_NUMBER, nullptr},
    {"release",  Rps10::RELEASE_HOLD, Rps10::RELEASES, Rps10::RELEASE_HOLD,
     PARAM_ENUM, RELEASE_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, 4, PARAMS}};

static const char* const IN_NAMES[2] = {"notes in", "root"};
static const char* const OUT_NAMES[1] = {"tone"};

const AlgorithmDescriptor Rps10::descriptor = {
    ALGO_RPS10, "RPS10", 2, 1, 1, 4, IN, OUT, sizeof(Rps10), false, construct_node<Rps10>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Notes set a BOSS RPS-10's pitch shift by their interval from the root, through a tone jack.",
    CATEGORY_MIDI };

static uint8_t clamp_priority(uint8_t stored){
    if (stored == 0 || stored > Rps10::PRIORITIES) return Rps10::PRIORITY_LATEST;
    return stored;
}

static uint8_t clamp_bend(uint8_t stored){
    if (stored == 0) return Rps10::DEFAULT_BEND;
    return stored > Rps10::MAX_BEND ? (uint8_t)Rps10::MAX_BEND : stored;
}

Rps10::Rps10(const NodeConfig& config) :
    in(config.in_buses[0]),
    root_in(config.in_buses[1]),
    tone_out(config.out_buses[0]),
    priority(clamp_priority(config.params[0])),
    octave(config.params[1] > KEY_MAX_OCTAVE ? (uint8_t)0 : config.params[1]),
    bend_semis(clamp_bend(config.params[2])),
    release(config.params[3] > RELEASES ? (uint8_t)RELEASE_HOLD : config.params[3]),
    played_root(NO_ROOT),
    bend_value(BEND_CENTRE),
    shift_semis(0),
    has_shift(false),
    tone_cv(0),
    held()
{}

uint8_t Rps10::root() const {
    return played_root != NO_ROOT ? global_key::placed(played_root, octave)
                                  : global_key::tonic(octave);
}

void Rps10::process(BusManager& bus, uint32_t){
    // The root first, so a root and a note arriving in the same pass agree.
    const uint8_t rn = bus.note_count(root_in);
    for (uint8_t i = 0; i < rn; i++){
        const MidiEvent e = bus.note_read(root_in, i);
        if (is_note_on(e)) played_root = e.data1;
    }

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_on(e)){
            // This node owns no note-off, so an eviction needs nothing.
            HeldNote evicted = {0, 0, 0};
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
        } else if (is_note_off(e)){
            held.remove(e.data1);
        } else if (e.type == MIDI_PITCH_BEND){
            bend_value = (uint16_t)(((uint16_t)(e.data2 & 0x7F) << 7) | (e.data1 & 0x7F));
        }
    }

    const uint8_t want = held.winner(rule());
    if (want != HeldNotes::NONE){
        int16_t semis = (int16_t)((int16_t)want - (int16_t)root());
        if (semis > MAX_SHIFT) semis = MAX_SHIFT;
        if (semis < -(int16_t)MAX_SHIFT) semis = -(int16_t)MAX_SHIFT;
        shift_semis = (int8_t)semis;
        has_shift = true;
    } else if (release == RELEASE_UNISON){
        shift_semis = 0;
    }

    const bool silent = want == HeldNotes::NONE && release == RELEASE_OFF;
    if (silent){
        tone_cv = 0;
    } else {
        const int16_t shift = has_shift ? shift_semis : (int16_t)0;
        int32_t t = (int32_t)tone_of_note(UNISON_NOTE) + (int32_t)shift * TONE_PER_SEMITONE;
        if (bend_semis != 0){
            const int32_t from_centre = (int32_t)bend_value - (int32_t)BEND_CENTRE;   // -8192 .. 8191
            t += (from_centre * TONE_PER_SEMITONE * (int32_t)bend_semis) / (int32_t)BEND_CENTRE;
        }
        // Bend can carry a held shift past the unit's span; the edge of it
        // is where the pitch stops.
        const int32_t lo = tone_of_note(UNISON_NOTE - MAX_SHIFT);
        const int32_t hi = tone_of_note(UNISON_NOTE + MAX_SHIFT);
        tone_cv = (int16_t)(t < lo ? lo : t > hi ? hi : t);
    }

    // Written every pass: a CV bus is summed and cleared by the swap.
    bus.cv_write(tone_out, tone_cv);
}

bool Rps10::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > PRIORITIES) return false;
            priority = value;
            return true;
        case 1:
            // A held shift is re-read from the held note next pass, so moving
            // the root's register - or the key - under a held key moves the
            // pitch.
            if (value > KEY_MAX_OCTAVE) return false;
            octave = value;
            return true;
        case 2:
            if (value > MAX_BEND) return false;
            bend_semis = value;
            return true;
        case 3:
            if (value > RELEASES) return false;
            release = value;
            return true;
        default:
            return false;
    }
}

uint8_t Rps10::get_param(uint16_t index) const {
    switch (index){
        case 0: return priority;
        case 1: return octave;
        case 2: return bend_semis;
        case 3: return release;
        default: return 0;
    }
}
