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
// **`diatonic` moves the part in the key's steps instead of in semitones**,
// so everything it plays is in the key the module is in (midi/global_key.h).
// The controls do not change: the semitone interval is read as the nearest
// interval the scale actually has, and an octave is a whole scale's worth of
// degrees, which is the same octave in any scale. So +4 is a third in both
// keys - a major one in C major and a minor one in C minor - and one setting
// plays the part right in whichever key the patch has moved to, which a fixed
// number of semitones cannot do.
//
// **It is not a quantiser bolted on the end**, and that difference is the
// whole reason it is a control here rather than a NoteQuantise patched after
// it. Snapping a shifted pitch pulls two notes a semitone apart onto the same
// degree and the line loses a step; moving every note by the same number of
// scale steps keeps its shape and lands all of it in the key. midi/scale.h
// argues the same point about degrees stored in a pattern.
//
// A note the key does not contain has no degree of its own, so it is put in
// the key first - and the shift applies to where it landed.
//
// It looks stateless and is not. Change any control while notes are held and
// a note-off computed from the *new* shift never matches the note-on already
// sent: the note hangs on the downstream synth. So the shifted note is
// recorded when it is emitted and released from that record, whatever the
// controls say by then (see midi/sounding_notes.h). The key itself may move
// under a held note for the same reason and with the same answer.
//
// A note that would land outside 0..127 is **dropped, not clamped** - and so
// is its note-off, by construction, because nothing was recorded for it.
// Clamping would pile the top of an octave doubler into unison at 127, which
// sounds like a bug rather than like a musical decision.
//
// params[0] semitones, params[1] octaves, both PARAM_CENTRED bytes: the value
// is the byte less PARAM_CENTRE, so a zeroed preset means no shift at all and
// a knob sweeping either one sweeps a single rising interval (node/param.h).
// params[2] diatonic: move in the key's steps rather than in semitones.
// params[3] channel: 0 keeps the channel each note arrived on (midi/note_event.h).
class Transpose : public Node{
    public:
        static constexpr uint16_t P_SEMITONES = 0, P_OCTAVES = 1, P_DIATONIC = 2,
                                  P_CHANNEL = 3;
        static constexpr uint16_t N_PARAMS = 4;
        static const int8_t MAX_SEMITONES = 12;
        static const int8_t MAX_OCTAVES = 4;

        // The note a shift drops, and what shift_note() answers with. Not a
        // pitch: 0xFF is not a MIDI note.
        static constexpr uint8_t DROPPED = 0xFF;

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
        void set_diatonic(bool value){ diatonic = value; }
        // The chromatic shift both controls ask for, in semitones. What the
        // node applies when `diatonic` is off, and the interval it reads as a
        // number of scale steps when it is on.
        int16_t offset() const { return (int16_t)semitones + (int16_t)octaves * 12; }
        // Where one note lands, or DROPPED when the shift puts it off the
        // keyboard.
        uint8_t shift_note(uint8_t note) const;
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        // The shift in scale steps: what `offset()` asks for, read in the key.
        int16_t degree_shift() const;

        uint8_t in;
        uint8_t out;
        int8_t semitones;
        int8_t octaves;
        bool diatonic;
        uint8_t channel;          // 0 keeps the source's
        SoundingNotes sounding;
};

#endif
