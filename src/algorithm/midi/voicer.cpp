#include "algorithm/midi/voicer.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const MODE_NAMES[Voicer::VOICE_MODES] = {"closest", "root", "drop 2", "spread"};

static const ParamDescriptor PARAMS[Voicer::N_PARAMS] = {
    {"mode",      Voicer::VOICE_CLOSEST, Voicer::VOICE_MODES, Voicer::VOICE_CLOSEST,
                  PARAM_ENUM, MODE_NAMES},
    {"low",       0, 127, Voicer::DEFAULT_LOW,  PARAM_PITCH,  nullptr},
    {"high",      0, 127, Voicer::DEFAULT_HIGH, PARAM_PITCH,  nullptr},
    {"voices",    0, Voicer::MAX_VOICES, 0,     PARAM_NUMBER, nullptr},
    {"bass",      0, 1, 0, PARAM_BOOL, nullptr},
    {"retrigger", 0, 1, 0, PARAM_BOOL, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, Voicer::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"chord in"};
static const char* const OUT_NAMES[1] = {"chord out"};

const AlgorithmDescriptor Voicer::descriptor = {
    ALGO_VOICER, "Voicer", 1, 1, 1, Voicer::N_PARAMS, IN, OUT, sizeof(Voicer), false,
    construct_node<Voicer>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Places a chord so the voices move as little as they can, and holds the notes it shares.",
    CATEGORY_MIDI };

// The bottom of the range. Capped an octave below the top of MIDI so that
// every pitch class always has somewhere to go: a range narrower than an
// octave cannot hold one, and place() would have nothing to return.
static constexpr uint8_t LOW_CEILING = 115;

uint8_t Voicer::range_high() const {
    const uint8_t bottom = range_low();
    uint16_t top = high;
    if (top < (uint16_t)bottom + 12) top = (uint16_t)bottom + 12;
    if (top > 127) top = 127;
    return (uint8_t)top;
}

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Voicer::Voicer(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    mode(clamp_enum(config.params[P_MODE], VOICE_MODES, VOICE_CLOSEST)),
    low(config.params[P_LOW] ? config.params[P_LOW] : DEFAULT_LOW),
    high(config.params[P_HIGH] ? config.params[P_HIGH] : DEFAULT_HIGH),
    voices(config.params[P_VOICES] > MAX_VOICES ? MAX_VOICES : config.params[P_VOICES]),
    bass(config.params[P_BASS] != 0),
    retrigger(config.params[P_RETRIGGER] != 0),
    held(), velocity(100), channel(1), voiced(), n_voiced(0), dirty(false), sounding()
{}

// Placement is the only thing every parameter here changes, so all of them
// but `retrigger` re-voice. `retrigger` says what the *next* chord change
// does and changes nothing about this one.
bool Voicer::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_MODE:
            if (value == 0 || value > VOICE_MODES) return false;
            mode = value; dirty = true; return true;
        case P_LOW:
            low = value ? value : DEFAULT_LOW; dirty = true; return true;
        case P_HIGH:
            high = value ? value : DEFAULT_HIGH; dirty = true; return true;
        case P_VOICES:
            if (value > MAX_VOICES) return false;
            voices = value; dirty = true; return true;
        case P_BASS:
            if (value > 1) return false;
            bass = value != 0; dirty = true; return true;
        case P_RETRIGGER:
            if (value > 1) return false;
            retrigger = value != 0; return true;
        default: return false;
    }
}

uint8_t Voicer::get_param(uint16_t index) const {
    switch (index){
        case P_MODE:      return mode;
        case P_LOW:       return low;
        case P_HIGH:      return high;
        case P_VOICES:    return voices;
        case P_BASS:      return bass ? 1u : 0u;
        case P_RETRIGGER: return retrigger ? 1u : 0u;
        default: return 0;
    }
}

uint8_t Voicer::place(uint8_t pc, uint8_t anchor) const {
    const int16_t bottom = (int16_t)range_low();
    const int16_t top = (int16_t)range_high();
    int16_t p = (int16_t)(pc % 12u);
    // The representative of this class nearest `anchor`; a tie goes up, which
    // is the same rule scale_quantise uses.
    while (p + 12 <= (int16_t)anchor + 6) p += 12;
    while (p < bottom) p += 12;
    while (p > top) p -= 12;
    return (uint8_t)p;
}

uint8_t Voicer::stack(uint8_t pc, uint8_t floor_note) const {
    int16_t p = (int16_t)(pc % 12u);
    while (p < (int16_t)floor_note) p += 12;
    while (p > 127) p -= 12;
    return (uint8_t)p;
}

uint8_t Voicer::range_low() const {
    return low > LOW_CEILING ? LOW_CEILING : low;
}

static void sort_ascending(uint8_t* notes, uint8_t n){
    for (uint8_t i = 1; i < n; i++){
        const uint8_t value = notes[i];
        uint8_t j = i;
        while (j > 0 && notes[j - 1] > value){ notes[j] = notes[j - 1]; j--; }
        notes[j] = value;
    }
}

uint8_t Voicer::voicing(uint8_t* want) const {
    const uint8_t n = held.count();
    if (n == 0) return 0;

    // The chord's pitch classes, in the order they appear from its own bass
    // upward. A doubled class is one voice: this node places classes, and two
    // voices on the same pitch is a unison nobody asked for.
    uint8_t pc[MAX_VOICES];
    uint8_t count = 0;
    uint16_t seen = 0;
    for (uint8_t i = 0; i < n && count < MAX_VOICES; i++){
        const uint8_t c = (uint8_t)(held.sorted(i).note % 12u);
        if (seen & (uint16_t)(1u << c)) continue;
        seen |= (uint16_t)(1u << c);
        pc[count++] = c;
    }
    if (voices && voices < count) count = voices;

    const uint8_t bottom = range_low();
    const bool lead = (mode == VOICE_CLOSEST) && n_voiced > 0;

    if (!lead){
        // Nothing to be near: stack it up from the bottom of the range. This
        // is also what `root`, `drop 2` and `spread` always do.
        const uint8_t gap = (mode == VOICE_SPREAD) ? SPREAD_GAP : 1u;
        uint8_t cursor = bottom;
        for (uint8_t i = 0; i < count; i++){
            want[i] = stack(pc[i], cursor);
            cursor = (uint8_t)(want[i] + gap > 127 ? 127 : want[i] + gap);
        }
    } else {
        // Each class takes the pitch nearest to any voice already sounding.
        // A class the chord shares with the last one is nearest to itself, at
        // distance zero, so a common tone lands exactly where it already is.
        for (uint8_t i = 0; i < count; i++){
            uint8_t best = place(pc[i], voiced[0]);
            uint16_t best_distance = (uint16_t)(best > voiced[0] ? best - voiced[0] : voiced[0] - best);
            for (uint8_t v = 1; v < n_voiced; v++){
                const uint8_t candidate = place(pc[i], voiced[v]);
                const uint16_t distance = (uint16_t)(candidate > voiced[v] ? candidate - voiced[v]
                                                                          : voiced[v] - candidate);
                if (distance < best_distance || (distance == best_distance && candidate < best)){
                    best = candidate;
                    best_distance = distance;
                }
            }
            want[i] = best;
        }
    }

    if (mode == VOICE_DROP2 && count >= 2){
        // The standard open voicing: the second voice from the top, an octave
        // down. Deliberately not voice-led - a fixed shape is what it is for.
        uint8_t ordered[MAX_VOICES];
        for (uint8_t i = 0; i < count; i++) ordered[i] = want[i];
        sort_ascending(ordered, count);
        if (ordered[count - 2] >= 12) ordered[count - 2] = (uint8_t)(ordered[count - 2] - 12);
        for (uint8_t i = 0; i < count; i++) want[i] = ordered[i];
    } else if (bass && count >= 2){
        // The chord's own bass, placed near where the bass was, and every
        // other voice forced above it. This is the setting that makes the
        // root motion audible, and it is the one that costs motion.
        want[0] = place(pc[0], n_voiced ? voiced[0] : bottom);
        for (uint8_t i = 1; i < count; i++){
            while (want[i] <= want[0] && want[i] + 12 <= 127) want[i] = (uint8_t)(want[i] + 12);
        }
    }

    sort_ascending(want, count);
    return count;
}

void Voicer::revoice(BusManager& bus){
    uint8_t want[MAX_VOICES];
    const uint8_t m = voicing(want);

    if (retrigger){
        sounding.release_all(bus, out);
    } else {
        // Everything sounding that the new chord does not want. Collected
        // first, because releasing compacts the ledger under the loop.
        uint8_t drop[MAX_VOICES];
        uint8_t n_drop = 0;
        for (uint8_t i = 0; i < sounding.count() && n_drop < MAX_VOICES; i++){
            const uint8_t note = sounding.at(i).note;
            bool keep = false;
            for (uint8_t j = 0; j < m; j++){ if (want[j] == note){ keep = true; break; } }
            if (!keep) drop[n_drop++] = note;
        }
        for (uint8_t i = 0; i < n_drop; i++) sounding.release(bus, out, drop[i]);
    }

    // The ledger is keyed on the emitted note, so `holds` asks the only
    // question a voicer has: is this pitch already in the air? A common tone
    // is neither released above nor emitted here.
    for (uint8_t j = 0; j < m; j++){
        if (sounding.holds(want[j])) continue;
        sounding.emit(bus, out, want[j], want[j], velocity, channel);
    }

    for (uint8_t j = 0; j < m; j++) voiced[j] = want[j];
    n_voiced = m;
}

void Voicer::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    bool changed = false;
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_on(e)){
            HeldNote evicted;
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
            if (e.data2) velocity = e.data2;
            if (e.channel) channel = e.channel;
            changed = true;
        } else if (is_note_off(e)){
            if (held.remove(e.data1)) changed = true;
        } else {
            bus.note_write(out, e);
        }
    }
    // A chord is what is held at the end of the pass. `Chord` releases its old
    // voices and sounds the new ones in one pass, so reading the events as
    // they arrive would voice the silence in between.
    if (!changed && !dirty) return;
    revoice(bus);
    dirty = false;
}

void Voicer::silence(BusManager& bus){
    sounding.release_all(bus, out);
    // Nothing is in the air, so the next pass voices from scratch rather than
    // believing a chord that has been taken down.
    n_voiced = 0;
}
