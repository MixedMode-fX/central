#ifndef MMMC_ALGORITHM_NOTE_QUANTISE_H
#define MMMC_ALGORITHM_NOTE_QUANTISE_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Snaps incoming pitches to the nearest tone of a scale.
//
// "Note quantise", not "quantise": this module quantises two unrelated
// things - a pitch to a scale, and a patch swap to a bar (#11) - and a name
// that does not say which is a name a user has to guess at.
//
// The general-purpose complement to degree-based sequencing: it tames a
// random source, an arpeggio spread over a wide chord, or a CV input, where
// the pitch that arrives is arbitrary rather than a degree that was chosen.
//
// It has no parameters at all: what it snaps to is the key the module is in
// (midi/global_key.h), and the only thing left to say about it is where the
// root comes from - which is a cable, not a setting. A patched root inlet
// outranks the key, because a cable is the most explicit thing a user can
// say; last note-on wins, so one keyboard transposes a whole patch.
//
// Changing the root or the scale under a sounding note is the failure this
// class of algorithm is prone to: the release is taken from the ledger, so it
// is the pitch that was actually sent, never a re-quantised one - and the
// same is true when the key itself moves under it.
//
// Inlet 0 (note): the notes to quantise.
// Inlet 1 (note, optional): the root. Note-ons set it; nothing else is read.
class NoteQuantise : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit NoteQuantise(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;

        // The scale and root actually played: the key's, unless a cable has
        // said otherwise.
        uint16_t active_mask() const;
        uint8_t active_root() const;

    private:
        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        // What the root inlet last wrote, or NO_ROOT until it has: a cable
        // that has not played anything yet has not said anything, so the key
        // is still what the node is in.
        static constexpr uint8_t NO_ROOT = 0xFF;
        uint8_t root;
        SoundingNotes sounding;
};

#endif
