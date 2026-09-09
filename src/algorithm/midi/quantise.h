#ifndef MMMC_ALGORITHM_QUANTISE_H
#define MMMC_ALGORITHM_QUANTISE_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Snaps incoming pitches to the nearest tone of a scale.
//
// The general-purpose complement to degree-based sequencing: it tames a
// random source, an arpeggio spread over a wide chord, or a CV input, where
// the pitch that arrives is arbitrary rather than a degree that was chosen.
// The scale mask is the same 12-bit representation the note sequencers use,
// so a scale is defined once for the whole module (midi/scale.h).
//
// The root can come from a note bus, last note-on wins, so one keyboard can
// transpose a whole patch. Changing the root or the scale under a sounding
// note is the failure this class of algorithm is prone to: the release is
// taken from the ledger, so it is the pitch that was actually sent, never a
// re-quantised one.
//
// Inlet 0 (note): the notes to quantise.
// Inlet 1 (note, optional): the root. Note-ons set it; nothing else is read.
// params[0] scale id (see ScaleId)   params[1] root pitch class, when no root
//                                              inlet is patched
class Quantise : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Quantise(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;

        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // The typed spellings of set_param, kept because they read better in
        // a test - and what proves a root or scale change cannot strand a
        // sounding note.
        void set_scale(uint8_t id){ scale = id; }
        void set_root(uint8_t pitch_class){ root = (uint8_t)(pitch_class % 12u); }
        uint8_t root_note() const { return root; }

    private:
        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        uint8_t scale;
        uint8_t root;
        SoundingNotes sounding;
};

#endif
