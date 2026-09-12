#include "algorithm/midi/note_quantise.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_key.h"

static const Domain IN[2] = {Domain::Note, Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const char* const IN_NAMES[2] = {"notes in", "root"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor NoteQuantise::descriptor = {
    ALGO_NOTE_QUANTISE, "Note Quantise", 2, 1, 1, 0, IN, OUT, sizeof(NoteQuantise), false,
    construct_node<NoteQuantise>, nullptr, 0, IN_NAMES, OUT_NAMES,
    "Snaps every note into the key the module is in. The root inlet moves it.",
    CATEGORY_MIDI,
    true };   // reads_key: every pitch it plays comes from the key

NoteQuantise::NoteQuantise(const NodeConfig& config) :
    in(config.in_bus[0]),
    root_in(config.in_bus[1]),
    out(config.out_bus[0]),
    root(NO_ROOT),
    sounding()
{}

uint16_t NoteQuantise::active_mask() const {
    return global_key::mask();
}

// A patched root inlet wins outright - `root` is what it last wrote. With no
// cable, the key's.
uint8_t NoteQuantise::active_root() const {
    return root != NO_ROOT ? root : global_key::root();
}

void NoteQuantise::process(BusManager& bus, uint32_t){
    // The root first, so a root and a note arriving in the same pass agree.
    if (root_in != NO_BUS){
        const uint8_t rn = bus.note_count(root_in);
        for (uint8_t i = 0; i < rn; i++){
            const MidiEvent e = bus.note_read(root_in, i);
            if (is_note_on(e)) root = (uint8_t)(e.data1 % 12u);
        }
    }

    const uint16_t mask = active_mask();
    const uint8_t tonic = active_root();
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
        const uint8_t snapped = scale_quantise(e.data1, tonic, mask);
        // Two incoming pitches can snap to the same tone. The ledger keys on
        // the source note, so each of them still gets its own release.
        sounding.emit(bus, out, e.data1, snapped, e.data2, e.channel);
    }
}

void NoteQuantise::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
