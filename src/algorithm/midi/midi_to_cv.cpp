#include "algorithm/midi/midi_to_cv.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[5] = {Domain::CV, Domain::Gate, Domain::CV, Domain::CV, Domain::Gate};

static const char* const GATE_NAMES[2] = {"legato", "retrigger"};
static const char* const MOD_SOURCE_NAMES[2] = {"cc", "pressure"};

static const ParamDescriptor PARAMS[8] = {
    {"priority", MidiToCv::PRIORITY_LOWEST, MidiToCv::PRIORITIES, MidiToCv::PRIORITY_LATEST,
     PARAM_ENUM, PARAM_PRIORITY_NAMES},
    {"range",    1, MidiToCv::MAX_RANGE, MidiToCv::DEFAULT_RANGE, PARAM_NUMBER, nullptr},
    {"base",     0, 127, MidiToCv::DEFAULT_BASE, PARAM_PITCH, nullptr},
    {"bend",     0, MidiToCv::MAX_BEND, MidiToCv::DEFAULT_BEND, PARAM_NUMBER, nullptr},
    {"gate",     MidiToCv::GATE_LEGATO, MidiToCv::GATE_MODES, MidiToCv::GATE_LEGATO,
     PARAM_ENUM, GATE_NAMES},
    {"width",    0, 255, 0, PARAM_MILLIS, nullptr},
    {"mod src",  MidiToCv::MOD_CC, MidiToCv::MOD_SOURCES, MidiToCv::MOD_CC,
     PARAM_ENUM, MOD_SOURCE_NAMES},
    {"mod cc",   0, 127, MidiToCv::DEFAULT_MOD_CC, PARAM_NUMBER, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 8, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[5] = {"pitch", "gate", "velocity", "mod", "trigger"};

const AlgorithmDescriptor MidiToCv::descriptor = {
    ALGO_MIDI_TO_CV, "MidiToCV", 1, 1, 5, 8, IN, OUT, sizeof(MidiToCv), false, construct_node<MidiToCv>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Notes become one CV voice: pitch, gate, velocity, modulation and a trigger.",
    CATEGORY_MIDI };

// Sub-bits the pitch arithmetic is carried at. Bend is a fraction of a
// semitone and a semitone is only 34 bus units at the default range, so
// rounding to whole bus units before the bend is added would quantise a
// pitch wheel to about four steps a semitone. Eight sub-bits put the
// smallest step the node can take at 1/256 of a bus unit, which is far below
// what the twelve-bit bus - or the DAC behind it - can render.
static constexpr uint8_t PITCH_SUB = 8;

static uint8_t clamp_priority(uint8_t stored){
    if (stored == 0 || stored > MidiToCv::PRIORITIES) return MidiToCv::PRIORITY_LATEST;
    return stored;
}

static uint8_t clamp_range(uint8_t stored){
    if (stored == 0 || stored > MidiToCv::MAX_RANGE) return MidiToCv::DEFAULT_RANGE;
    return stored;
}

static uint8_t clamp_bend(uint8_t stored){
    if (stored == 0) return MidiToCv::DEFAULT_BEND;
    return stored > MidiToCv::MAX_BEND ? (uint8_t)MidiToCv::MAX_BEND : stored;
}

static uint8_t clamp_mod_source(uint8_t stored){
    if (stored == 0 || stored > MidiToCv::MOD_SOURCES) return MidiToCv::MOD_CC;
    return stored;
}

// A seven-bit controller value across the unipolar range.
static int16_t seven_bit_cv(uint8_t value){
    return (int16_t)(((int32_t)(value & 0x7F) * CV_MAX) / 127);
}

MidiToCv::MidiToCv(const NodeConfig& config) :
    in(config.in_bus[0]),
    pitch_out(config.out_bus[0]),
    gate_out(config.out_bus[1]),
    velocity_out(config.out_bus[2]),
    mod_out(config.out_bus[3]),
    trig_out(config.out_bus[4]),
    priority(clamp_priority(config.params[0])),
    range(clamp_range(config.params[1])),
    base(config.params[2] ? config.params[2] : (uint8_t)DEFAULT_BASE),
    bend_semis(clamp_bend(config.params[3])),
    gate_mode(config.params[4] > GATE_MODES ? (uint8_t)GATE_LEGATO : config.params[4]),
    width_param(config.params[5]),
    mod_src(clamp_mod_source(config.params[6])),
    mod_cc(config.params[7] ? config.params[7] : (uint8_t)DEFAULT_MOD_CC),
    per_semitone(0),
    bend_value(BEND_CENTRE),
    playing(HeldNotes::NONE),
    // The pitch bus has a level before the first note arrives, so it starts
    // at the bottom of the range rather than wherever note zero happens to
    // sit under the current base.
    last_note(base),
    pitch_cv(0), velocity_cv(0), mod_cv(0),
    gate_level(false),
    held(), pulse()
{
    derive();
    if (width_param) pulse.set_width_us((uint32_t)width_param * 1000u);
}

void MidiToCv::derive(){
    // Full scale is `range` octaves, so a semitone is CV_FULL / (12 * range).
    per_semitone = (int32_t)(((uint32_t)CV_FULL << PITCH_SUB) / (12u * (uint32_t)range));
}

int16_t MidiToCv::pitch_of(uint8_t note) const {
    int32_t q = ((int32_t)note - (int32_t)base) * per_semitone;
    if (bend_semis != 0){
        const int32_t from_centre = (int32_t)bend_value - (int32_t)BEND_CENTRE;   // -8192 .. 8191
        // Divided before the second multiply, so the whole expression stays
        // inside an int32 without a 64-bit multiply on every pass. What that
        // drops is under a 256th of a bus unit per semitone of bend range -
        // three orders of magnitude below one step of the bus it lands on.
        q += ((from_centre * per_semitone) / (int32_t)BEND_CENTRE) * (int32_t)bend_semis;
    }
    // A note below `base` has nowhere to go: the range has a bottom, and a
    // pitch that wrapped round to the top of it would be a wrong note rather
    // than a flat one.
    return (int16_t)cv_clamp_unipolar(q >> PITCH_SUB);
}

void MidiToCv::process(BusManager& bus, uint32_t now_us){
    // Which notes arrived this pass, so that "the voice changed" and "a key
    // was struck" can be told apart below. A bitmap rather than a list
    // because a pass can carry a whole chord and only its size is bounded.
    uint32_t attacked[4] = {0, 0, 0, 0};

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_on(e)){
            // The eviction is not acted on: this node owns no note-off, and
            // the voice is re-derived from what is held either way.
            HeldNote evicted = {0, 0, 0};
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
            attacked[(e.data1 >> 5) & 3] |= (uint32_t)1 << (e.data1 & 31);
        } else if (is_note_off(e)){
            held.remove(e.data1);
        } else if (e.type == MIDI_PITCH_BEND){
            bend_value = (uint16_t)(((uint16_t)(e.data2 & 0x7F) << 7) | (e.data1 & 0x7F));
        } else if (e.type == MIDI_CONTROL_CHANGE && mod_src == MOD_CC && e.data1 == mod_cc){
            mod_cv = seven_bit_cv(e.data2);
        } else if (e.type == MIDI_AFTERTOUCH_CHANNEL && mod_src == MOD_PRESSURE){
            mod_cv = seven_bit_cv(e.data1);
        }
    }

    const uint8_t want = held.winner(rule());
    // An attack is a note-on this pass that *took* the voice. A note-off that
    // hands the voice back to a note still held is not one: releasing the top
    // of a legato line must not re-strike the envelope underneath it.
    const bool attack = want != HeldNotes::NONE &&
                        (attacked[(want >> 5) & 3] & ((uint32_t)1 << (want & 31))) != 0;
    const bool was_gated = playing != HeldNotes::NONE;

    if (want != playing || attack){
        playing = want;
        if (want != HeldNotes::NONE){
            const HeldNote* h = held.find(want);
            last_note = want;
            velocity_cv = seven_bit_cv(h ? h->velocity : 0);
        }
    }
    if (attack) pulse.fire(now_us);

    // Recomputed every pass rather than only when the voice moves: bend runs
    // under a held note, and so do `range` and `base` when a hand is on them.
    pitch_cv = pitch_of(last_note);
    gate_level = playing != HeldNotes::NONE &&
                 !(attack && was_gated && gate_mode == GATE_RETRIGGER);

    // Every outlet is written every pass: a CV bus is summed and cleared by
    // the swap, and a gate bus is a level only for as long as somebody holds
    // it up. An outlet left at NO_BUS goes nowhere, which is how a patch
    // takes the gate and ignores the rest.
    bus.cv_write(pitch_out, pitch_cv);
    bus.gate_write(gate_out, gate_level);
    bus.cv_write(velocity_out, velocity_cv);
    bus.cv_write(mod_out, mod_cv);
    bus.gate_write(trig_out, pulse.level(now_us));
}

bool MidiToCv::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > PRIORITIES) return false;
            priority = value;
            return true;
        case 1:
            if (value == 0 || value > MAX_RANGE) return false;
            if (value == range) return true;
            range = value;
            derive();
            return true;
        case 2:
            base = value & 0x7F;
            return true;
        case 3:
            if (value > MAX_BEND) return false;
            bend_semis = value;
            return true;
        case 4:
            if (value > GATE_MODES) return false;
            gate_mode = value;
            return true;
        case 5:
            width_param = value;
            pulse.set_width_us(value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US);
            return true;
        case 6:
            if (value == 0 || value > MOD_SOURCES) return false;
            // The level the previous source left is kept: dropping a
            // modulation to zero because a parameter moved is a click.
            mod_src = value;
            return true;
        case 7:
            mod_cc = value & 0x7F;
            return true;
        default:
            return false;
    }
}

uint8_t MidiToCv::get_param(uint16_t index) const {
    switch (index){
        case 0: return priority;
        case 1: return range;
        case 2: return base;
        case 3: return bend_semis;
        case 4: return gate_mode;
        case 5: return width_param;
        case 6: return mod_src;
        case 7: return mod_cc;
        default: return 0;
    }
}
