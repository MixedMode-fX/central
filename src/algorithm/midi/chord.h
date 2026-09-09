#ifndef MMMC_ALGORITHM_CHORD_H
#define MMMC_ALGORITHM_CHORD_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// One note in, a chord out: the root plus an interval set.
//
// The first modifier where one inlet event becomes several outlet events, so
// it is the one that tests the note bus's queue depth and the ledger's
// capacity. A voice that cannot be recorded is not emitted at all, because a
// note this node cannot release is a note that hangs for ever.
//
// **The intervals are steps of a scale**, not semitones - which is the same
// thing in the chromatic scale, where one step is one semitone, and that is
// what an unset scale used to be. So 0 2 4 is a triad: major on C in the
// major scale, minor on D in the same scale, and the plain 0 2 4 semitones
// it always was when the scale is chromatic. A stack of fixed semitones is
// still a stack of fixed semitones: name the chromatic scale on this node
// and nothing follows the key.
//
// The scale is the module's own (midi/global_scale.h) unless this node names
// one - the same rule NoteQuantise follows, including the root: following
// the module's scale means following its key, naming a scale here means this
// node's root parameter is the key, and a patched root inlet outranks both.
//
// A played note the scale does not contain is snapped into it first (the
// same snap NoteQuantise does), so the chord is in key even when the playing
// is not, and every voice is measured from the note that was actually
// emitted.
//
// Intervals outside 0..127 are dropped, like Transpose and for the same
// reason. Interval 0 is the root; a chord with no intervals configured is
// the root alone, which makes an unconfigured Chord a pass-through rather
// than a silence.
//
// Inlet 0 (note): the note to voice.
// Inlet 1 (note, optional): the root of the key. Note-ons set it.
//
// params[0] count      how many of the intervals below are used (0 -> root only)
// params[1..6] intervals, signed scale steps from the root
// params[7] scale id (see ScaleId; 0 follows the module's scale)
// params[8] root pitch class, when this node names its own scale and no root
//           inlet is patched
class Chord : public Node{
    public:
        static constexpr uint8_t MAX_INTERVALS = 6;
        static constexpr uint16_t P_SCALE = 7, P_ROOT = 8;

        static const AlgorithmDescriptor descriptor;
        explicit Chord(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }
        // The scale and root actually played, after the module's own have
        // been resolved into them.
        uint16_t active_mask() const;
        uint8_t active_root() const;

    private:
        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        uint8_t n_intervals;
        uint8_t scale;
        uint8_t root;
        int8_t intervals[MAX_INTERVALS];
        SoundingNotes sounding;
};

#endif
