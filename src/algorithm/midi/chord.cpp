#include "algorithm/midi/chord.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_scale.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

// The named stacks, in scale steps from the root. The root itself is emitted
// by emit_chord and is not listed, which is why every one of these starts at
// the second voice.
struct QualityStack { uint8_t count; int8_t step[Chord::MAX_INTERVALS]; };
static const QualityStack QUALITY[Chord::QUALITY_COUNT - 1] = {
    {2, {2, 4}},          // triad
    {3, {2, 4, 6}},       // 7th
    {4, {2, 4, 6, 8}},    // 9th
    {3, {2, 4, 5}},       // 6th
    {2, {1, 4}},          // sus2
    {2, {3, 4}},          // sus4
    {2, {3, 6}},          // quartal
    {2, {2, 6}},          // shell: the root, the third and the seventh
    {1, {4}},             // fifth
};

static_assert(sizeof(QUALITY) / sizeof(QUALITY[0]) == Chord::QUALITY_COUNT - 1,
              "one interval stack per named quality, and none for `custom`");

static const char* const QUALITY_NAMES[Chord::QUALITY_COUNT] = {
    "custom", "triad", "7th", "9th", "6th", "sus2", "sus4", "quartal", "shell", "fifth",
};

static const ParamDescriptor PARAMS[Chord::N_PARAMS] = {
    {"voices",     0, Chord::MAX_INTERVALS, 0, PARAM_NUMBER, nullptr},
    {"interval 1", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 2", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 3", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 4", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 5", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"interval 6", 0, 255, 0, PARAM_SIGNED, nullptr},
    {"scale",      0, SCALE_COUNT - 1, 0, PARAM_ENUM,        PARAM_SCALE_NAMES},
    {"root",       0, 11,              0, PARAM_PITCH_CLASS, nullptr},
    {"octave",     0, Chord::MAX_OCTAVE, Chord::DEFAULT_OCTAVE,   PARAM_NUMBER, nullptr},
    {"velocity",   1, 127,               Chord::DEFAULT_VELOCITY, PARAM_NUMBER, nullptr},
    {"quality",    0, Chord::QUALITY_COUNT - 1, Chord::QUALITY_CUSTOM, PARAM_ENUM, QUALITY_NAMES},
    {"retrigger",  0, 1,                 0, PARAM_BOOL, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, Chord::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"note in", "root"};
static const char* const OUT_NAMES[1] = {"chord out"};

// min_in is 0: with nothing patched to `note in` the node plays its own
// chord and holds it (see the header).
const AlgorithmDescriptor Chord::descriptor = {
    ALGO_CHORD, "Chord", 2, 0, 1, Chord::N_PARAMS, IN, OUT, sizeof(Chord), false, construct_node<Chord>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A chord from one note - or from none: unpatched it plays and holds its own, in the key.",
    CATEGORY_MIDI };

// The voicing can move under a held chord: every voice already in the air is
// released from the ledger at the pitch it was emitted at, so re-voicing -
// or a change of scale, here or on the module - changes what the *next*
// note-on plays and strands nothing.
bool Chord::set_param(uint16_t index, uint8_t value){
    if (index == 0){
        if (value > MAX_INTERVALS) return false;
        n_intervals = value;
        dirty = true;
        return true;
    }
    if (index >= 1 && index <= MAX_INTERVALS){
        intervals[index - 1] = (int8_t)value;
        dirty = true;
        return true;
    }
    if (index == P_SCALE){
        if (value >= SCALE_COUNT) return false;
        scale = value;
        dirty = true;
        return true;
    }
    if (index == P_ROOT){
        root = (uint8_t)(value % 12u);
        dirty = true;
        return true;
    }
    if (index == P_OCTAVE){
        if (value > MAX_OCTAVE) return false;
        octave = value;
        dirty = true;
        return true;
    }
    if (index == P_VELOCITY){
        if (value > 127) return false;
        velocity = value ? value : DEFAULT_VELOCITY;
        dirty = true;
        return true;
    }
    if (index == P_QUALITY){
        if (value >= QUALITY_COUNT) return false;
        quality = value;
        dirty = true;
        return true;
    }
    if (index == P_RETRIGGER){
        if (value > 1) return false;
        // Not a re-voice: it says what the *next* root note-on does.
        retrigger = value != 0;
        return true;
    }
    return false;
}

uint8_t Chord::get_param(uint16_t index) const {
    if (index == 0) return n_intervals;
    if (index >= 1 && index <= MAX_INTERVALS) return (uint8_t)intervals[index - 1];
    if (index == P_SCALE) return scale;
    if (index == P_ROOT) return root;
    if (index == P_OCTAVE) return octave;
    if (index == P_VELOCITY) return velocity;
    if (index == P_QUALITY) return quality;
    if (index == P_RETRIGGER) return retrigger ? 1u : 0u;
    return 0;
}

Chord::Chord(const NodeConfig& config) :
    in(config.in_bus[0]),
    root_in(config.in_bus[1]),
    out(config.out_bus[0]),
    n_intervals(config.params[0] > MAX_INTERVALS ? MAX_INTERVALS : config.params[0]),
    scale(config.params[P_SCALE]),
    root((uint8_t)(config.params[P_ROOT] % 12u)),
    octave(config.params[P_OCTAVE] ? config.params[P_OCTAVE] : DEFAULT_OCTAVE),
    velocity(config.params[P_VELOCITY] ? config.params[P_VELOCITY] : DEFAULT_VELOCITY),
    quality(config.params[P_QUALITY] < QUALITY_COUNT ? config.params[P_QUALITY] : (uint8_t)QUALITY_CUSTOM),
    retrigger(config.params[P_RETRIGGER] != 0),
    free_note(NO_NOTE), voiced(NO_NOTE), free_channel(1), voiced_mask(0), dirty(false),
    intervals(), sounding()
{
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) intervals[i] = (int8_t)config.params[i + 1];
}

uint16_t Chord::active_mask() const {
    return global_scale::resolve_id(scale);
}

uint8_t Chord::active_root() const {
    // A patched root inlet names the key - except on a self-playing node,
    // where it is the thing playing the chord and names the note instead, so
    // the chords a sequencer walks through stay in one key rather than
    // dragging the key along with them.
    if (root_in != NO_BUS && in != NO_BUS) return root;
    return global_scale::resolve_root(scale, root);
}

// A named quality is read from the table; `custom` plays what was typed. The
// typed intervals are never overwritten by a quality, so switching back to
// `custom` finds the hand-built stack exactly as it was left.
const int8_t* Chord::active_intervals(uint8_t& count) const {
    if (quality == QUALITY_CUSTOM || quality >= QUALITY_COUNT){
        count = n_intervals;
        return intervals;
    }
    const QualityStack& stack = QUALITY[quality - 1];
    count = stack.count;
    return stack.step;
}

// Every voice of one chord, recorded against `source` so that one release
// takes all of it down at the pitches it was actually sent at.
void Chord::emit_chord(BusManager& bus, uint8_t source, uint8_t base, uint8_t key,
                       uint16_t mask, uint8_t velocity_out, uint8_t channel){
    const int16_t degree = semitone_to_scale_degree((int16_t)((int16_t)base - (int16_t)key), mask);
    uint8_t count = 0;
    const int8_t* steps = active_intervals(count);
    sounding.emit(bus, out, source, base, velocity_out, channel);
    for (uint8_t v = 0; v < count; v++){
        const int16_t note = (int16_t)key
                           + scale_degree_to_semitone((int16_t)(degree + steps[v]), mask);
        if (note < 0 || note > 127) continue;
        if (note == base) continue;                                       // no unisons
        sounding.emit(bus, out, source, (uint8_t)note, velocity_out, channel);
    }
}

// A chord with nothing patched to `note in`: it plays itself, and keeps
// playing. The pass costs one comparison once the chord is up - the work only
// happens when what it should be sounding has actually moved.
void Chord::play_free(BusManager& bus, uint16_t mask, uint8_t key){
    // The root inlet places it if anything does; otherwise it is the tonic of
    // the key, in the configured octave.
    // Its own octave, unless the key names a register - in which case a
    // self-playing chord sits where the module says home is, and `octave`
    // is what it falls back to. The root inlet still outranks both.
    const uint8_t own = (uint8_t)(((int16_t)key + (int16_t)octave * 12) > 127
                                  ? 127 : ((int16_t)key + (int16_t)octave * 12));
    int16_t wanted = (free_note != NO_NOTE) ? (int16_t)free_note
                                            : (int16_t)global_scale::resolve_anchor_id(scale, own);
    while (wanted > 127) wanted -= 12;               // dropped an octave, never wrapped
    const uint8_t base = scale_quantise((uint8_t)wanted, key, mask);

    if (!dirty && base == voiced && mask == voiced_mask) return;
    // Re-voicing is a release and a new chord, both from the ledger, so
    // editing one while it drones cannot strand a note.
    sounding.release_all(bus, out);
    emit_chord(bus, base, base, key, mask, velocity, free_channel);
    voiced = base;
    voiced_mask = mask;
    dirty = false;
}

void Chord::process(BusManager& bus, uint32_t){
    const bool self_playing = (in == NO_BUS);

    // The root first, so a root and a note arriving in the same pass agree,
    // as in NoteQuantise.
    if (root_in != NO_BUS){
        const uint8_t rn = bus.note_count(root_in);
        for (uint8_t i = 0; i < rn; i++){
            const MidiEvent e = bus.note_read(root_in, i);
            if (!is_note_on(e)) continue;
            if (self_playing){
                // Nothing else is playing this node, so this is what does:
                // the whole note, octave and all, on the channel it arrived
                // on. A root that names the note already sounding leaves it
                // alone unless `retrigger` says otherwise: the chord is held,
                // and a held chord is not re-struck for nothing.
                free_note = e.data1;
                free_channel = e.channel ? e.channel : 1u;
                if (retrigger) dirty = true;
            } else {
                root = (uint8_t)(e.data1 % 12u);
            }
        }
    }

    const uint16_t mask = active_mask();
    const uint8_t key = active_root();
    if (self_playing){
        play_free(bus, mask, key);
        return;
    }

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);
            continue;
        }
        if (!is_note_on(e)){
            bus.note_write(out, e);
            continue;
        }
        // The root voice, in key. In the chromatic scale this is the note
        // itself and every interval below is a semitone, which is what this
        // algorithm did before it had a scale.
        emit_chord(bus, e.data1, scale_quantise(e.data1, key, mask), key, mask, e.data2, e.channel);
    }
}

void Chord::silence(BusManager& bus){
    sounding.release_all(bus, out);
    // Nothing is sounding, so a self-playing node voices again on its next
    // pass rather than believing a chord that has been taken down.
    voiced = NO_NOTE;
}
