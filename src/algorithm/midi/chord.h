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
// Intervals outside 0..127 are dropped, like Transpose and for the same
// reason. Interval 0 is the root; a chord with no intervals configured is
// the root alone, which makes an unconfigured Chord a pass-through rather
// than a silence.
//
// params[0] count      how many of the intervals below are used (0 -> root only)
// params[1..6] intervals, signed semitones from the root
class Chord : public Node{
    public:
        static constexpr uint8_t MAX_INTERVALS = 6;

        static const AlgorithmDescriptor descriptor;
        explicit Chord(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;

        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }

    private:
        uint8_t in;
        uint8_t out;
        uint8_t n_intervals;
        int8_t intervals[MAX_INTERVALS];
        SoundingNotes sounding;
};

#endif
