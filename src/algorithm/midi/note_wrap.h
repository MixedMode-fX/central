#ifndef MMMC_ALGORITHM_MIDI_NOTE_WRAP_H
#define MMMC_ALGORITHM_MIDI_NOTE_WRAP_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Keeps every note inside a range by moving it whole octaves: a note above
// the top comes down, a note below the bottom goes up, by as few octaves as
// bring it inside, so it keeps its pitch class. A note already inside passes
// untouched.
//
// **The range hangs off the root.** `low` is where it starts, in semitones
// from the root, and `range` is how far up it reaches from there, both edges
// included: the default is the root to the major seventh above it, one
// octave, so every pitch class has exactly one place and a keyboard of any
// width plays one octave's worth of notes. The root is the key's tonic
// (midi/global_key.h), in the key's register unless `octave` names one; a
// patched root inlet outranks the key, as on every node that has one: its
// last note-on is the root, in its own register unless `octave` names one
// (global_key::placed), and it holds until the next.
//
// **A range under an octave has pitch classes with no place.** No octave
// brings them inside, so they land on the nearest edge after the fold, as
// `RPS10`'s wrap does: a note always comes out, and it is always in range.
//
// The range is clipped to the keyboard, so a root near the top or bottom
// shortens it rather than folding notes off the end.
//
// Every note-off releases what its note-on actually sent (midi/sounding_notes.h),
// so the root, the key and the range may all move under a held note.
//
// Inlet 0 (note): the notes to wrap. Anything that is not a note passes through.
// Inlet 1 (note, optional): the root. Note-ons set it; nothing else is read.
// Outlet 0 (note): the wrapped notes.
//
// params[0] octave   the root's register: 0 the key's (or the played root's), 1..10 one of its own
// params[1] low      the bottom of the range, semitones from the root (PARAM_CENTRED)
// params[2] range    semitones from the bottom of the range to the top (0 = DEFAULT_RANGE)
// params[3] channel  0 keeps the channel each note arrived on (midi/note_event.h)
class NoteWrap : public Node{
    public:
        static constexpr uint16_t P_OCTAVE = 0, P_LOW = 1, P_RANGE = 2, P_CHANNEL = 3;
        static constexpr uint8_t N_PARAMS = 4;
        static constexpr int8_t MAX_LOW = 48;
        static constexpr uint8_t DEFAULT_RANGE = 11;   // one octave, each pitch class once
        static constexpr uint8_t MAX_RANGE = 96;
        static constexpr uint8_t NO_ROOT = 0xFF;

        static const AlgorithmDescriptor descriptor;

        explicit NoteWrap(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        // The pitch the range is measured from this pass.
        uint8_t root() const;
        // The range's edges, inclusive, on the keyboard.
        uint8_t bottom() const;
        uint8_t top() const;
        // Where one note lands.
        uint8_t wrap(uint8_t note) const;
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        BusSet in;
        BusSet root_in;
        BusSet out;
        uint8_t octave;         // 0 is the key's register, or the played root's
        int8_t low;
        uint8_t range;
        uint8_t channel;        // 0 keeps the source's
        uint8_t played_root;    // the root inlet's last note-on, NO_ROOT until one
        SoundingNotes sounding;
};

#endif
