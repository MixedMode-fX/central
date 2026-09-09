#ifndef MMMC_ALGORITHM_TRANSPOSE_H
#define MMMC_ALGORITHM_TRANSPOSE_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Shifts every note by a number of semitones; everything else passes through.
//
// It looks stateless and is not. Change the offset while notes are held and a
// note-off computed from the *new* offset never matches the note-on already
// sent: the note hangs on the downstream synth. So the shifted note is
// recorded when it is emitted and released from that record, whatever the
// offset says by then (see midi/sounding_notes.h).
//
// A note that would land outside 0..127 is **dropped, not clamped** - and so
// is its note-off, by construction, because nothing was recorded for it.
// Clamping would pile the top of an octave doubler into unison at 127, which
// sounds like a bug rather than like a musical decision.
//
// params[0] semitones, as a signed 8-bit value stored in the byte
class Transpose : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit Transpose(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;

        // Live parameter edits arrive with the patch protocol (#11); this is
        // the seam they will use, and what makes "the offset moved while
        // notes were held" a test rather than a promise.
        void set_semitones(int8_t value){ semitones = value; }
        int8_t offset() const { return semitones; }
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        uint8_t in;
        uint8_t out;
        int8_t semitones;
        SoundingNotes sounding;
};

#endif
