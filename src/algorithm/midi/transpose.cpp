#include "algorithm/midi/transpose.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_key.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[Transpose::N_PARAMS] = {
    {"semitones", PARAM_CENTRE - Transpose::MAX_SEMITONES, PARAM_CENTRE + Transpose::MAX_SEMITONES,
                  PARAM_CENTRE, PARAM_CENTRED, nullptr},
    {"octaves",   PARAM_CENTRE - Transpose::MAX_OCTAVES,   PARAM_CENTRE + Transpose::MAX_OCTAVES,
                  PARAM_CENTRE, PARAM_CENTRED, nullptr},
    {"diatonic",  0, 1, 0, PARAM_BOOL, nullptr},
    {"channel",   0, 16, 0, PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, Transpose::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Transpose::descriptor = {
    ALGO_TRANSPOSE, "Transpose", 1, 1, 1, Transpose::N_PARAMS, IN, OUT, sizeof(Transpose), false,
    construct_node<Transpose>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Shifts every note up or down, by semitones or by the key's own steps. Out of 0..127 is dropped.",
    CATEGORY_MIDI,
    true };   // reads_key: `diatonic` moves the part in the key's steps

// Clamped rather than refused, because this runs on a stored byte: a preset
// saved when the bounds were wider loads playing something close to what it
// said, where a refusal would lose the whole patch. A *runtime* write outside
// the bounds is a different thing and is rejected (set_param below), because
// there is a caller there to tell.
static int8_t clamp_to(uint8_t stored, int8_t limit){
    const int8_t value = param_centred(stored);
    if (value > limit) return limit;
    if (value < -limit) return -limit;
    return value;
}

Transpose::Transpose(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    semitones(clamp_to(config.params[P_SEMITONES], MAX_SEMITONES)),
    octaves(clamp_to(config.params[P_OCTAVES], MAX_OCTAVES)),
    diatonic(config.params[P_DIATONIC] != 0),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    sounding()
{}

// **The nearest interval the key actually has, in scale steps.** A semitone
// count is an interval a player names - a third, a fifth - and most of them
// are not intervals of the scale: a major third is four semitones and the
// minor key has no note there. So the ceiling and the floor of the interval
// are both measured and the nearer one wins, which is how +4 is a third in
// both keys - a major one in C major, a minor one in C minor.
//
// A tie goes to the smaller interval, because the note it picks is the one
// the interval is named after: +4 in a minor key is between the minor third
// and the fourth, and a player who asked for a third means the third. The
// exception is the tie at nothing - +1 semitone in any seven-note scale -
// where a shift that was asked for is never rounded away to none.
static int16_t nearest_degree(int16_t semitones, uint16_t mask){
    if (semitones == 0) return 0;
    // semitone_to_scale_degree is the ceiling: the smallest degree at or
    // above the interval. One below it is therefore the floor.
    const int16_t over = semitone_to_scale_degree(semitones, mask);
    const int16_t under = over - 1;
    const int16_t above = scale_degree_to_semitone(over, mask) - semitones;
    const int16_t below = semitones - scale_degree_to_semitone(under, mask);
    const int16_t chosen = (above < below) ? over : under;
    if (chosen != 0) return chosen;
    return semitones > 0 ? (int16_t)1 : (int16_t)-1;
}

// What `offset()` asks for, in the key's steps: the semitone interval read as
// the nearest interval of the scale, plus a whole scale's worth of degrees
// per octave - which is the octave, in any scale, because that is what a
// scale repeats at.
int16_t Transpose::degree_shift() const {
    const uint16_t mask = global_key::mask();
    return (int16_t)(nearest_degree(semitones, mask)
                     + (int16_t)octaves * (int16_t)scale_size(mask));
}

uint8_t Transpose::shift_note(uint8_t note) const {
    int16_t shifted;
    if (diatonic){
        const uint16_t mask = global_key::mask();
        const int16_t root = (int16_t)global_key::root();
        // A note the key does not contain has no degree of its own, so it is
        // put in the key first - nearest, not rounded up, which is the one
        // decision scale_quantise already makes for every other node.
        const uint8_t snapped = scale_quantise(note, (uint8_t)root, mask);
        // Measured from the key's root as a pitch class, so a note below it
        // is a negative offset and a negative degree: both maps floor, so the
        // two round-trip either side of the root.
        const int16_t degree = semitone_to_scale_degree((int16_t)snapped - root, mask);
        shifted = (int16_t)(root + scale_degree_to_semitone(degree + degree_shift(), mask));
    } else {
        shifted = (int16_t)note + offset();
    }
    if (shifted < 0 || shifted > 127) return DROPPED;
    return (uint8_t)shifted;
}

void Transpose::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);      // as sent, not as configured
            continue;
        }
        if (is_note_on(e)){
            const uint8_t shifted = shift_note(e.data1);
            if (shifted == DROPPED) continue;         // dropped, off and all
            sounding.emit(bus, out, e.data1, shifted, e.data2,
                          out_channel(channel, e.channel));
            continue;
        }
        bus.note_write(out, readdressed(e, channel));
    }
}

bool Transpose::set_param(uint16_t index, uint8_t value){
    // The ledger releases at the sent pitch, so any control may move under a
    // held note.
    const int8_t wanted = param_centred(value);
    switch (index){
        case P_SEMITONES:
            if (wanted > MAX_SEMITONES || wanted < -MAX_SEMITONES) return false;
            semitones = wanted;
            return true;
        case P_OCTAVES:
            if (wanted > MAX_OCTAVES || wanted < -MAX_OCTAVES) return false;
            octaves = wanted;
            return true;
        case P_DIATONIC:
            if (value > 1) return false;
            diatonic = value != 0;
            return true;
        case P_CHANNEL:
            if (value > 16) return false;
            channel = value;
            return true;
        default:
            return false;
    }
}

uint8_t Transpose::get_param(uint16_t index) const {
    switch (index){
        case P_SEMITONES: return param_centred_byte(semitones);
        case P_OCTAVES:   return param_centred_byte(octaves);
        case P_DIATONIC:  return diatonic ? 1u : 0u;
        case P_CHANNEL:   return channel;
        default:          return 0;
    }
}

void Transpose::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
