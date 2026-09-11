#include "algorithm/midi/mirror.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_scale.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const MODE_NAMES[Mirror::MIRROR_MODES] = {"negative", "inversion"};

static const ParamDescriptor PARAMS[Mirror::N_PARAMS] = {
    {"mode",   Mirror::MIRROR_NEGATIVE, Mirror::MIRROR_MODES, Mirror::MIRROR_NEGATIVE,
               PARAM_ENUM, MODE_NAMES},
    {"scale",  0, SCALE_COUNT - 1, 0, PARAM_ENUM,        PARAM_SCALE_NAMES},
    {"root",   0, 11,              0, PARAM_PITCH_CLASS, nullptr},
    {"amount", 1, 100, Mirror::DEFAULT_AMOUNT, PARAM_PERCENT, nullptr},
    {"snap",   0, 1,   0, PARAM_BOOL,   nullptr},
    {"seed",   0, 255, 0, PARAM_NUMBER, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, Mirror::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[2] = {"notes in", "axis root"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Mirror::descriptor = {
    ALGO_MIRROR, "Mirror", 2, 1, 1, Mirror::N_PARAMS, IN, OUT, sizeof(Mirror), false,
    construct_node<Mirror>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Negative harmony: reflects every note about the key's axis, so a progression plays its shadow.",
    CATEGORY_MIDI };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Mirror::Mirror(const NodeConfig& config) :
    in(config.in_bus[0]),
    root_in(config.in_bus[1]),
    out(config.out_bus[0]),
    mode(clamp_enum(config.params[P_MODE], MIRROR_MODES, MIRROR_NEGATIVE)),
    scale(config.params[P_SCALE]),
    root((uint8_t)(config.params[P_ROOT] % 12u)),
    amount(config.params[P_AMOUNT] ? (config.params[P_AMOUNT] > 100 ? (uint8_t)100
                                                                    : config.params[P_AMOUNT])
                                   : DEFAULT_AMOUNT),
    snap(config.params[P_SNAP] != 0),
    seed(config.params[P_SEED]),
    // Seeded from entropy when `seed` is zero and from the byte otherwise, so
    // a patch can be exactly reproducible or never the same twice.
    rng(config.params[P_SEED] ? (uint32_t)(config.params[P_SEED] * 2654435761u) : entropy::seed()),
    sounding()
{}

// Nothing here can strand a note: the release is taken from the ledger, so it
// is the pitch that was actually sent and never a re-reflected one. That is
// what lets the axis, the mode and the scale all move under a sounding note.
bool Mirror::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_MODE:
            if (value == 0 || value > MIRROR_MODES) return false;
            mode = value; return true;
        case P_SCALE:
            if (value >= SCALE_COUNT) return false;
            scale = value; return true;
        case P_ROOT:
            root = (uint8_t)(value % 12u); return true;
        case P_AMOUNT:
            if (value > 100) return false;
            amount = value ? value : DEFAULT_AMOUNT; return true;
        case P_SNAP:
            if (value > 1) return false;
            snap = value != 0; return true;
        case P_SEED:
            // There is no reset inlet here, so a new seed takes effect at
            // once: the stream this node draws from is the only state it has.
            seed = value;
            rng.reseed(value ? (uint32_t)(value * 2654435761u) : entropy::seed());
            return true;
        default: return false;
    }
}

uint8_t Mirror::get_param(uint16_t index) const {
    switch (index){
        case P_MODE:   return mode;
        case P_SCALE:  return scale;
        case P_ROOT:   return root;
        case P_AMOUNT: return amount;
        case P_SNAP:   return snap ? 1u : 0u;
        case P_SEED:   return seed;
        default: return 0;
    }
}

uint16_t Mirror::active_mask() const {
    return global_scale::resolve_id(scale);
}

// A patched root inlet wins outright - `root` is what it last wrote. With no
// cable, following the module's scale means following its root too.
uint8_t Mirror::active_root() const {
    if (root_in != NO_BUS) return root;
    return global_scale::resolve_root(scale, root);
}

uint8_t Mirror::reflect(uint8_t note) const {
    const uint8_t key = active_root();
    // Negative harmony reflects about the axis halfway between the tonic and
    // the dominant, which relative to the tonic is `7 - x`; inversion
    // reflects about the tonic itself. Both are done on the pitch class,
    // because reflecting the pitch lands nowhere near the keyboard.
    const int16_t offset = (int16_t)((int16_t)note - (int16_t)key);
    const int16_t axis = (mode == MIRROR_NEGATIVE) ? 7 : 0;
    int16_t pc = (int16_t)(((axis - offset) % 12 + 12) % 12);
    int16_t pitch = (int16_t)key % 12 + pc;
    // The octave nearest the note that caused it, so a reflection sits among
    // the notes that passed through rather than under them.
    while (pitch + 12 <= (int16_t)note + 6) pitch += 12;
    while (pitch > (int16_t)note + 6) pitch -= 12;
    if (pitch < 0 || pitch > 127) return 0xFF;
    if (snap) return scale_quantise((uint8_t)pitch, key, active_mask());
    return (uint8_t)pitch;
}

void Mirror::process(BusManager& bus, uint32_t){
    // The root first, so a root and a note arriving in the same pass agree,
    // as in NoteQuantise.
    if (root_in != NO_BUS){
        const uint8_t rn = bus.note_count(root_in);
        for (uint8_t i = 0; i < rn; i++){
            const MidiEvent e = bus.note_read(root_in, i);
            if (is_note_on(e)) root = (uint8_t)(e.data1 % 12u);
        }
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
        // A note that is not reflected still goes through the ledger, so the
        // note-off it owes is the one this node sent whichever way the coin
        // came down - the coin is flipped once, at the note-on, and never
        // again.
        const uint8_t note = rng.chance(amount) ? reflect(e.data1) : e.data1;
        if (note == 0xFF) continue;                    // dropped, and its note-off with it
        sounding.emit(bus, out, e.data1, note, e.data2, e.channel);
    }
}

void Mirror::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
