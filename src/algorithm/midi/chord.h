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
// **With nothing patched to `note in` the chord plays itself.** A voicer that
// needs a keyboard is a voicer that cannot start a patch, and "set the notes
// and let it run" is what a module with no keys attached is for: unpatched,
// the node sounds its chord once - the tonic of whatever key it is in, at
// `octave` - and holds it, indefinitely, with no clock and no player. That
// is exactly the shape an arpeggiator downstream wants, because an
// arpeggiator arpeggiates held notes and does not care whose fingers are
// holding them.
//
// A held chord is not a silent one: it is **re-voiced** whenever what it
// should be playing changes - a note-on on the root inlet (so a sequencer
// moves the chord a bar at a time), the key changing under it, or any
// parameter of the voicing being edited. Re-voicing releases every note it
// had sounding from the ledger and sounds the new chord, so a chord editable
// while it drones cannot strand a note, and a downstream arpeggiator simply
// sees the figure change.
//
// The root inlet is what a self-playing chord is *played* by, so on one it
// means what `note in` means everywhere else: the whole note rather than only
// its pitch class - a sequencer sends C3 and the chord moves to C3, octave
// and all - and it does not touch the key. That is the difference between a
// sequenced root walking through the chords of one key and one dragging the
// key along behind it, and only the first is a chord progression. With a note
// inlet patched the root inlet means what it always did: the key's root, and
// the pitch comes from the notes.
//
// Inlet 0 (note, optional): the note to voice. Unpatched, the node plays
//         itself, as above.
// Inlet 1 (note, optional): the root. Note-ons set it - the key's root
//         normally, the note to play on a self-playing node.
//
// params[0] count      how many of the intervals below are used (0 -> root only)
// params[1..6] intervals, signed scale steps from the root
// params[7] scale id (see ScaleId; 0 follows the module's scale)
// params[8] root pitch class, when this node names its own scale and no root
//           inlet is patched
// params[9] octave     where a self-playing chord sits: its root is
//                      12 x octave + the key's root (0 -> DEFAULT_OCTAVE,
//                      middle C). Ignored while a note inlet is patched.
// params[10] velocity  what a self-playing chord is sounded at. Ignored
//                      while a note inlet is patched: a played note keeps
//                      the velocity it was played with.
class Chord : public Node{
    public:
        static constexpr uint8_t MAX_INTERVALS = 6;
        static constexpr uint16_t P_SCALE = 7, P_ROOT = 8, P_OCTAVE = 9, P_VELOCITY = 10;
        static constexpr uint8_t N_PARAMS = 11;
        // Middle C is 12 x 5: the octave a chord nobody has placed should
        // sound in, and the velocity a note nobody played should sound at.
        static constexpr uint8_t DEFAULT_OCTAVE = 5, MAX_OCTAVE = 10, DEFAULT_VELOCITY = 100;
        // No note: what `free_note` holds until the root inlet names one, and
        // what `voiced` holds while nothing is sounding.
        static constexpr uint8_t NO_NOTE = 0xFF;

        static const AlgorithmDescriptor descriptor;
        explicit Chord(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t sounding_count() const { return sounding.count(); }
        // True when this node plays itself: nothing is patched to `note in`.
        bool free_running() const { return in == NO_BUS; }
        // The root a self-playing chord is sounding, or NO_NOTE.
        uint8_t voiced_note() const { return voiced; }
        uint32_t refused() const { return sounding.refused(); }
        // The scale and root actually played, after the module's own have
        // been resolved into them.
        uint16_t active_mask() const;
        uint8_t active_root() const;

    private:
        // Sounds the root plus every interval, all recorded against `source`
        // so one release takes the whole chord down.
        void emit_chord(BusManager& bus, uint8_t source, uint8_t base, uint8_t key,
                        uint16_t mask, uint8_t velocity_out, uint8_t channel);
        // One pass of a chord with no note inlet: works out what it should be
        // playing and re-voices only if that has moved.
        void play_free(BusManager& bus, uint16_t mask, uint8_t key);

        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        uint8_t n_intervals;
        uint8_t scale;
        uint8_t root;
        uint8_t octave;
        uint8_t velocity;
        // Free-running state. `free_note` is the note the root inlet last
        // named (NO_NOTE: derive it from the key and the octave), `voiced`
        // the root actually sounding, `voiced_mask` the scale it was voiced
        // in, and `dirty` a parameter edit that has to be heard.
        uint8_t free_note;
        uint8_t voiced;
        uint8_t free_channel;
        uint16_t voiced_mask;
        bool dirty;
        int8_t intervals[MAX_INTERVALS];
        SoundingNotes sounding;
};

#endif
