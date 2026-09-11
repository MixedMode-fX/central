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
// one, and the root is the module's own unless the `key` parameter says
// otherwise - two questions, two parameters, the same rule NoteQuantise
// follows. A patched root inlet outranks both.
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
// **`quality` names a stack so the intervals do not have to be typed.** Six
// signed scale steps is the general case and a poor default: a triad, a
// seventh and a ninth are the three chords almost every patch wants, and in
// scale steps they are 2 4, 2 4 6 and 2 4 6 8 - short enough that typing them
// is not hard and frequent enough that nobody should have to. While `quality`
// names one, the `voices` and `interval` parameters are ignored rather than
// overwritten, so a hand-built stack is still there when it is set back to
// `custom`. Every named stack is degrees, like the typed ones, so `7th` on
// degree 5 of a major key is a minor seventh and on degree 4 a dominant
// seventh, with nothing anywhere naming either.
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
// parameter of the voicing being edited.
//
// **A root that repeats is not a change, and `retrigger` is what says
// otherwise.** A held chord that re-struck itself every time a sequencer
// resent the note it is already playing would be a chord nobody could drone
// on, so by default it does not. But `Harmony` repeats a degree whenever its
// style or its `gravity` says so, and there the silence is a hole in the
// progression rather than a held note - one chord of the phrase simply does
// not sound. So it is a switch, defaulting to the held behaviour, which is
// what a stored zero has always meant here. Re-voicing releases every note it
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
// params[8] root pitch class, when this node names its own key and no root
//           inlet is patched
// params[9] octave     where a self-playing chord sits: its root is
//                      12 x octave + the key's root (0 -> DEFAULT_OCTAVE,
//                      middle C), and when the key names a register of its
//                      own this is how far from it the chord plays. Ignored
//                      while a note inlet is patched.
// params[10] velocity  what a self-playing chord is sounded at. Ignored
//                      while a note inlet is patched: a played note keeps
//                      the velocity it was played with.
// params[11] quality   a named stack of scale steps, or `custom` (0) to use
//                      the intervals above.
// params[12] retrigger re-strike the chord on every root note-on, including
//                      one that names the note already sounding.
// params[13] key       follow the module's root, or use this node's own. The
//                      scale is a separate parameter and a separate question:
//                      a chord can voice a mode of its own without leaving
//                      the key (midi/global_scale.h).
class Chord : public Node{
    public:
        static constexpr uint8_t MAX_INTERVALS = 6;
        static constexpr uint16_t P_SCALE = 7, P_ROOT = 8, P_OCTAVE = 9, P_VELOCITY = 10,
                                  P_QUALITY = 11, P_RETRIGGER = 12, P_KEY = 13;
        static constexpr uint8_t N_PARAMS = 14;

        // Named interval stacks, in scale steps from the root. `custom` is 0
        // so that a preset written before this existed - and any preset that
        // never mentions quality - keeps playing the intervals it stored.
        enum Quality : uint8_t {
            QUALITY_CUSTOM  = 0,
            QUALITY_TRIAD   = 1,
            QUALITY_SEVENTH = 2,
            QUALITY_NINTH   = 3,
            QUALITY_SIXTH   = 4,
            QUALITY_SUS2    = 5,
            QUALITY_SUS4    = 6,
            QUALITY_QUARTAL = 7,
            QUALITY_SHELL   = 8,
            QUALITY_FIFTH   = 9,
            QUALITY_COUNT   = 10,
        };
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
        // The stack actually voiced: the named quality's steps, or the typed
        // intervals when quality is `custom`. `count` is how many are used.
        const int8_t* active_intervals(uint8_t& count) const;
        // The scale and root actually played, after the module's own have
        // been resolved into them.
        uint16_t active_mask() const;
        uint8_t active_root() const;

    private:
        // Sounds the root plus every interval, all recorded against `source`
        // so one release takes the whole chord down.
        void emit_chord(BusManager& bus, uint8_t source, uint8_t base, uint8_t tonic,
                        uint16_t mask, uint8_t velocity_out, uint8_t channel);
        // One pass of a chord with no note inlet: works out what it should be
        // playing and re-voices only if that has moved.
        void play_free(BusManager& bus, uint16_t mask, uint8_t tonic);

        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        uint8_t n_intervals;
        uint8_t scale;
        uint8_t root;
        uint8_t octave;
        uint8_t key;
        uint8_t velocity;
        uint8_t quality;
        bool retrigger;
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
