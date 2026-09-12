#include "algorithm/midi/chord.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_scale.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

// The named stacks. The root itself is emitted by emit_chord and is not
// listed, which is why every one of these starts at the second voice.
//
// `step` is the stack in scale steps, which is what makes a quality diatonic:
// the same two steps are a major triad on one degree of the key and a minor
// triad on the next. `semitone` is the same chord written out, and is what a
// chromatic key plays, because twelve steps of twelve notes have no degree to
// take a flavour from - a triad of them would be three adjacent semitones.
struct QualityStack {
    uint8_t count;
    int8_t step[Chord::MAX_STEPS];
    int8_t semitone[Chord::MAX_STEPS];
};
static const QualityStack QUALITY[Chord::QUALITY_COUNT - Chord::QUALITY_TRIAD] = {
    {2, {2, 4},       {4, 7}},          // triad
    {3, {2, 4, 6},    {4, 7, 10}},      // 7th
    {4, {2, 4, 6, 8}, {4, 7, 10, 14}},  // 9th
    {3, {2, 4, 5},    {4, 7, 9}},       // 6th
    {2, {1, 4},       {2, 7}},          // sus2
    {2, {3, 4},       {5, 7}},          // sus4
    {2, {3, 6},       {5, 10}},         // quartal
    {2, {2, 6},       {4, 10}},         // shell: the root, the third and the seventh
    {1, {4},          {7}},             // fifth
};

static_assert(sizeof(QUALITY) / sizeof(QUALITY[0]) == Chord::QUALITY_COUNT - Chord::QUALITY_TRIAD,
              "one interval stack per named quality");

// Indexed from the parameter's own minimum, which is QUALITY_TRIAD.
static const char* const QUALITY_NAMES[Chord::QUALITY_COUNT - Chord::QUALITY_TRIAD] = {
    "triad", "7th", "9th", "6th", "sus2", "sus4", "quartal", "shell", "fifth",
};
static const char* const VOICING_NAMES[Chord::VOICING_COUNT] = {
    "close", "open", "drop 2", "drop 3", "wide",
};
static const char* const INVERSION_NAMES[Chord::MAX_INVERSION + 1] = {
    "root pos", "1st", "2nd", "3rd",
};

// What the chord is, then where it sits, then the key it is in - the three
// key parameters together, because "A minor, following the module" is one
// decision and not three.
static const ParamDescriptor PARAMS[Chord::N_PARAMS] = {
    {"quality",   Chord::QUALITY_TRIAD, Chord::QUALITY_COUNT - 1, Chord::QUALITY_TRIAD,
                                        PARAM_ENUM,        QUALITY_NAMES},
    {"voicing",   0, Chord::VOICING_COUNT - 1, Chord::VOICING_CLOSE, PARAM_ENUM, VOICING_NAMES},
    {"inversion", 0, Chord::MAX_INVERSION,     0,          PARAM_ENUM,        INVERSION_NAMES},
    {"key",       0, global_scale::KEY_MODES - 1, 0,       PARAM_ENUM,        PARAM_KEY_NAMES},
    {"root",      0, 11,              0,                   PARAM_PITCH_CLASS, nullptr},
    {"scale",     0, SCALE_COUNT - 1, 0,                   PARAM_ENUM,        PARAM_SCALE_NAMES},
    {"octave",    0, Chord::MAX_OCTAVE, Chord::DEFAULT_OCTAVE,   PARAM_NUMBER, nullptr},
    {"velocity",  1, 127,               Chord::DEFAULT_VELOCITY, PARAM_NUMBER, nullptr},
    {"retrigger", 0, 1,                 0,                       PARAM_BOOL,   nullptr},
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

// Ascending, in place. Never more than MAX_VOICES entries, and the array is
// nearly sorted every time it is called, which is what insertion sort is for.
static void sort_voices(int16_t* pitch, uint8_t n){
    for (uint8_t i = 1; i < n; i++){
        const int16_t x = pitch[i];
        uint8_t j = i;
        while (j > 0 && pitch[j - 1] > x){ pitch[j] = pitch[j - 1]; j--; }
        pitch[j] = x;
    }
}

// The voicing can move under a held chord: every voice already in the air is
// released from the ledger at the pitch it was emitted at, so re-voicing -
// or a change of scale, here or on the module - changes what the *next*
// note-on plays and strands nothing.
bool Chord::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_QUALITY:
            if (value < QUALITY_TRIAD || value >= QUALITY_COUNT) return false;
            quality = value;
            break;
        case P_VOICING:
            if (value >= VOICING_COUNT) return false;
            voicing = value;
            break;
        case P_INVERSION:
            if (value > MAX_INVERSION) return false;
            inversion = value;
            break;
        case P_KEY:
            if (value >= global_scale::KEY_MODES) return false;
            key = value;
            break;
        case P_ROOT:
            root = (uint8_t)(value % 12u);
            break;
        case P_SCALE:
            if (value >= SCALE_COUNT) return false;
            scale = value;
            break;
        case P_OCTAVE:
            if (value > MAX_OCTAVE) return false;
            octave = value;
            break;
        case P_VELOCITY:
            if (value > 127) return false;
            velocity = value ? value : DEFAULT_VELOCITY;
            break;
        case P_RETRIGGER:
            if (value > 1) return false;
            // Not a re-voice: it says what the *next* root note-on does.
            retrigger = value != 0;
            return true;
        default:
            return false;
    }
    dirty = true;
    return true;
}

uint8_t Chord::get_param(uint16_t index) const {
    switch (index){
        case P_QUALITY:   return quality;
        case P_VOICING:   return voicing;
        case P_INVERSION: return inversion;
        case P_KEY:       return key;
        case P_ROOT:      return root;
        case P_SCALE:     return scale;
        case P_OCTAVE:    return octave;
        case P_VELOCITY:  return velocity;
        case P_RETRIGGER: return retrigger ? 1u : 0u;
        default:          return 0;
    }
}

Chord::Chord(const NodeConfig& config) :
    in(config.in_bus[0]),
    root_in(config.in_bus[1]),
    out(config.out_bus[0]),
    quality(config.params[P_QUALITY] >= QUALITY_TRIAD && config.params[P_QUALITY] < QUALITY_COUNT
            ? config.params[P_QUALITY] : (uint8_t)QUALITY_TRIAD),
    voicing(config.params[P_VOICING] < VOICING_COUNT ? config.params[P_VOICING] : (uint8_t)VOICING_CLOSE),
    inversion(config.params[P_INVERSION] <= MAX_INVERSION ? config.params[P_INVERSION] : (uint8_t)0),
    key(config.params[P_KEY] < global_scale::KEY_MODES ? config.params[P_KEY] : (uint8_t)0),
    root((uint8_t)(config.params[P_ROOT] % 12u)),
    scale(config.params[P_SCALE]),
    octave(config.params[P_OCTAVE] ? config.params[P_OCTAVE] : DEFAULT_OCTAVE),
    velocity(config.params[P_VELOCITY] ? config.params[P_VELOCITY] : DEFAULT_VELOCITY),
    retrigger(config.params[P_RETRIGGER] != 0),
    free_note(NO_NOTE), voiced(NO_NOTE), free_channel(1), voiced_mask(0), dirty(false),
    sounding()
{}

uint16_t Chord::active_mask() const {
    return global_scale::resolve_id(scale);
}

uint8_t Chord::active_root() const {
    // A patched root inlet names the key - except on a self-playing node,
    // where it is the thing playing the chord and names the note instead, so
    // the chords a sequencer walks through stay in one key rather than
    // dragging the key along with them.
    if (root_in != NO_BUS && in != NO_BUS) return root;
    return global_scale::resolve_root(key, root);
}

// The quality's voices over `base`, ascending: every stack rises, and both
// maps below are monotonic, so nothing here has to sort.
uint8_t Chord::build(uint8_t base, uint8_t tonic, uint16_t mask, int16_t* pitch) const {
    const QualityStack& stack = QUALITY[quality - QUALITY_TRIAD];
    pitch[0] = (int16_t)base;
    if ((mask & 0x0FFF) == 0x0FFF){
        // Chromatic: no degrees, so the chord is its own shape in semitones.
        for (uint8_t v = 0; v < stack.count; v++)
            pitch[v + 1] = (int16_t)base + (int16_t)stack.semitone[v];
    } else {
        const int16_t degree = semitone_to_scale_degree((int16_t)((int16_t)base - (int16_t)tonic), mask);
        for (uint8_t v = 0; v < stack.count; v++)
            pitch[v + 1] = (int16_t)tonic
                         + scale_degree_to_semitone((int16_t)(degree + stack.step[v]), mask);
    }
    return (uint8_t)(stack.count + 1);
}

// Inversion first: which voice is in the bass is decided before the stack is
// opened out, so `drop 2` on a first inversion drops the second voice of the
// inversion rather than of the root position.
void Chord::shape(int16_t* pitch, uint8_t n) const {
    const uint8_t inv = inversion < n ? inversion : (uint8_t)(n - 1);
    for (uint8_t i = 0; i < inv; i++) pitch[i] += 12;
    sort_voices(pitch, n);

    switch (voicing){
        case VOICING_OPEN:
            for (uint8_t v = 1; v < n; v += 2) pitch[v] += 12;
            break;
        case VOICING_DROP2:
            if (n >= 2) pitch[n - 2] -= 12;
            break;
        case VOICING_DROP3:
            // The third from the top, or the second on a chord with no third.
            if (n >= 3) pitch[n - 3] -= 12;
            else if (n >= 2) pitch[n - 2] -= 12;
            break;
        case VOICING_WIDE:
            for (uint8_t v = 1; v < n; v++)
                while (pitch[v] < pitch[v - 1] + 12) pitch[v] += 12;
            break;
        default:
            break;
    }
    sort_voices(pitch, n);
}

// Every voice of one chord, recorded against `source` so that one release
// takes all of it down at the pitches it was actually sent at.
void Chord::emit_chord(BusManager& bus, uint8_t source, uint8_t base, uint8_t tonic,
                       uint16_t mask, uint8_t velocity_out, uint8_t channel){
    int16_t pitch[MAX_VOICES];
    const uint8_t n = build(base, tonic, mask, pitch);
    shape(pitch, n);
    for (uint8_t v = 0; v < n; v++){
        if (pitch[v] < 0 || pitch[v] > 127) continue;          // off the keyboard
        if (v > 0 && pitch[v] == pitch[v - 1]) continue;       // no unisons; sorted, so adjacent
        sounding.emit(bus, out, source, (uint8_t)pitch[v], velocity_out, channel);
    }
}

// A chord with nothing patched to `note in`: it plays itself, and keeps
// playing. The pass costs one comparison once the chord is up - the work only
// happens when what it should be sounding has actually moved.
void Chord::play_free(BusManager& bus, uint16_t mask, uint8_t tonic){
    // The root inlet places it if anything does; otherwise it is the tonic of
    // the key, in the octave this node names - and when the key names a
    // register of its own, `octave` says how far from that register the chord
    // sits, so one key moves every voice and each still keeps its place
    // (midi/global_scale.h). The root inlet outranks both.
    const uint8_t own = (uint8_t)(((int16_t)tonic + (int16_t)octave * 12) > 127
                                  ? 127 : ((int16_t)tonic + (int16_t)octave * 12));
    int16_t wanted = (free_note != NO_NOTE)
        ? (int16_t)free_note
        : (int16_t)global_scale::resolve_anchor(key, own, (uint8_t)(DEFAULT_OCTAVE * 12));
    while (wanted > 127) wanted -= 12;               // dropped an octave, never wrapped
    const uint8_t base = scale_quantise((uint8_t)wanted, tonic, mask);

    if (!dirty && base == voiced && mask == voiced_mask) return;
    // Re-voicing is a release and a new chord, both from the ledger, so
    // editing one while it drones cannot strand a note.
    sounding.release_all(bus, out);
    emit_chord(bus, base, base, tonic, mask, velocity, free_channel);
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
    const uint8_t tonic = active_root();
    if (self_playing){
        play_free(bus, mask, tonic);
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
        // The root voice, in key: the note itself in a chromatic one, and the
        // nearest note of the scale in any other.
        emit_chord(bus, e.data1, scale_quantise(e.data1, tonic, mask), tonic, mask, e.data2, e.channel);
    }
}

void Chord::silence(BusManager& bus){
    sounding.release_all(bus, out);
    // Nothing is sounding, so a self-playing node voices again on its next
    // pass rather than believing a chord that has been taken down.
    voiced = NO_NOTE;
}
