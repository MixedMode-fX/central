#ifndef MMMC_ALGORITHM_TRANSPOSE_H
#define MMMC_ALGORITHM_TRANSPOSE_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Shifts every note by a number of semitones, up or down; everything else
// passes through.
//
// **Two controls over one shift, because they are two different gestures.**
// `semitones` is the interval a part is moved by and is worth a whole knob
// within the octave it lives in; `octaves` is the register it plays in, and
// asking for it in twelves is asking the player to do arithmetic on a module
// they are trying to play. So the shift is `semitones + 12 * octaves` and the
// bounds are separate: +-MAX_SEMITONES and +-MAX_OCTAVES.
//
// It looks stateless and is not. Change either control while notes are held
// and a note-off computed from the *new* shift never matches the note-on
// already sent: the note hangs on the downstream synth. So the shifted note is
// recorded when it is emitted and released from that record, whatever the
// controls say by then (see midi/sounding_notes.h).
//
// A note that would land outside 0..127 is **dropped, not clamped** - and so
// is its note-off, by construction, because nothing was recorded for it.
// Clamping would pile the top of an octave doubler into unison at 127, which
// sounds like a bug rather than like a musical decision.
//
// params[0] semitones, params[1] octaves, both PARAM_CENTRED bytes: the value
// is the byte less PARAM_CENTRE, so a zeroed preset means no shift at all and
// a knob sweeping either one sweeps a single rising interval (node/param.h).
class Transpose : public Node{
    public:
        static const int8_t MAX_SEMITONES = 12;
        static const int8_t MAX_OCTAVES = 4;

        static const AlgorithmDescriptor descriptor;
        explicit Transpose(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;

        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // The typed spellings of set_param(n, ...), kept because they read
        // better in a test than a byte cast does.
        void set_semitones(int8_t value){ semitones = value; }
        void set_octaves(int8_t value){ octaves = value; }
        // What the node actually applies: both controls, in semitones.
        int16_t offset() const { return (int16_t)semitones + (int16_t)octaves * 12; }
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        uint8_t in;
        uint8_t out;
        int8_t semitones;
        int8_t octaves;
        SoundingNotes sounding;
};

#endif
