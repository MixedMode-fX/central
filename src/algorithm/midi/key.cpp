#include "algorithm/midi/key.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_key.h"

static const Domain IN[1] = {Domain::Note};

static const char* const FROM_NAMES[Key::KEY_FROMS] = {"pitch class", "note"};

static const ParamDescriptor PARAMS[Key::N_PARAMS] = {
    {"channel", 0, 16, 0, PARAM_CHANNEL, nullptr},
    {"from",    Key::KEY_FROM_PITCH_CLASS, Key::KEY_FROMS, Key::KEY_FROM_PITCH_CLASS,
                PARAM_ENUM, FROM_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, Key::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[1] = {"root"};

// No outlets: what it writes is the key, which is not a bus. The inlet is
// required, because a Key node with nothing patched to it moves nothing, and
// a module in the patch that silently does nothing is worse than a patch the
// validator refuses.
const AlgorithmDescriptor Key::descriptor = {
    ALGO_KEY, "Key", 1, 1, 0, Key::N_PARAMS, IN, nullptr, sizeof(Key), false,
    construct_node<Key>, GROUPS, 1, IN_NAMES, nullptr,
    "The key, in the patch: a note bus moves the root every node plays in.",
    CATEGORY_MIDI,
    false,      // reads_key: it writes the key, it does not play in it
    true,       // writes_key
    true };     // singleton

Key::Key(const NodeConfig& config) :
    root_in(config.in_bus[0]),
    channel(config.params[P_CHANNEL] > 16 ? (uint8_t)0 : config.params[P_CHANNEL]),
    from(config.params[P_FROM] >= KEY_FROM_PITCH_CLASS && config.params[P_FROM] <= KEY_FROMS
         ? config.params[P_FROM] : (uint8_t)KEY_FROM_PITCH_CLASS),
    move_count(0)
{}

bool Key::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_CHANNEL:
            if (value > 16) return false;
            channel = value; return true;
        case P_FROM:
            if (value == 0 || value > KEY_FROMS) return false;
            from = value; return true;
        default: return false;
    }
}

uint8_t Key::get_param(uint16_t index) const {
    switch (index){
        case P_CHANNEL: return channel;
        case P_FROM:    return from;
        default:        return 0;
    }
}

// Last note-on in the pass wins, which is the rule every root inlet in this
// module follows. Nothing is written when nothing arrives: the key stays
// where the last note left it, so a sequencer that moves it once a phrase
// does not have to hold a note for the rest of the phrase.
void Key::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(root_in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(root_in, i);
        if (!is_note_on(e)) continue;
        if (channel != 0 && e.channel != channel) continue;
        global_key::set_root((uint8_t)(e.data1 % 12u));
        // The register only when it was asked for. Notes 0..11 sit in an
        // octave the key cannot name - zero there means "the default"
        // everywhere else in the module - so they take the lowest one it can.
        if (from == KEY_FROM_NOTE){
            const uint8_t reg = (uint8_t)(e.data1 / 12u);
            global_key::set_octave(reg == 0 ? (uint8_t)1 : reg);
        }
        move_count++;
    }
}
