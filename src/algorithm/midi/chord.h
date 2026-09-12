#ifndef MMMC_ALGORITHM_CHORD_H
#define MMMC_ALGORITHM_CHORD_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// One note in, a chord out.
//
// The first modifier where one inlet event becomes several outlet events, so
// it is the one that tests the note bus's queue depth and the ledger's
// capacity. A voice that cannot be recorded is not emitted at all, because a
// note this node cannot release is a note that hangs for ever.
//
// **Three questions make a chord, and there is one parameter for each.**
// `quality` says what it is, `inversion` says which voice is in the bass, and
// `voicing` says how far apart they sit. Everything else on the node is about
// the chord it plays when nobody is playing it. Which notes it may use is not
// a question a chord answers: it is the key the module is in.
//
// **A quality is a stack of scale steps, so the key decides how it sounds.**
// `triad` is steps 2 and 4 above the root: major on the first degree of a
// major key, minor on the second, diminished on the seventh, and nothing
// anywhere names any of them. `7th` on the fifth degree is a dominant seventh
// and on the fourth a major seventh for the same reason. This is why
// `Harmony` only has to emit a root (algorithm/midi/harmony.h).
//
// A chromatic key has no degrees to take a flavour from - a triad of its
// steps is three adjacent semitones - so there each quality is its own shape
// in semitones instead, and `triad` is the major triad it has to be. That is
// the one place the two tables differ, and it is what makes an unconfigured
// `Chord` on a module nobody has set a key on play a chord rather than a
// cluster.
//
// **`inversion` moves the lowest voices up an octave**, one per step, so a
// triad reads root position, first, second. More inversions than the chord
// has voices is the highest one it has.
//
// **`voicing` is the shape of the stack**, applied after the inversion:
//   close   as stacked. Every voice inside one octave where the quality fits.
//   open    every other voice up an octave: the open position a piano plays
//           a triad in, and the one voicing that opens a chord without
//           moving its bass.
//   drop 2  the second voice from the top down an octave - the standard
//           four-part open voicing.
//   drop 3  the third from the top down instead; on a chord too small to
//           have one, the second.
//   wide    every voice at least an octave above the one below it, so a
//           triad covers three octaves.
// A voice pushed off either end of the keyboard is dropped rather than
// folded, and two voices landing on the same pitch sound once.
//
// The scale and the root are the key's (midi/global_key.h), and a patched
// root inlet outranks them - the same rule NoteQuantise follows.
//
// A played note the scale does not contain is snapped into it first (the
// same snap NoteQuantise does), so the chord is in key even when the playing
// is not, and every voice is measured from the note that was actually
// emitted.
//
// **With nothing patched to `note in` the chord plays itself.** A voicer that
// needs a keyboard is a voicer that cannot start a patch, and "set the notes
// and let it run" is what a module with no keys attached is for: unpatched,
// the node sounds its chord once - the root of whatever key it is in, in the
// register `octave` names - and holds it, indefinitely, with no clock and no player. That
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
// **A chord played by the root inlet is released when the transport stops.**
// It is held until the next root note-on, and a stopped clock is the one
// moment the module knows that note-on may never come (node/node.h) - the
// same reason Harmony releases its root there. The next root note-on brings
// it back. A chord with neither inlet patched has no edge to wait for and is
// not a chord the transport was ever driving, so it drones through the stop.
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
// params[0] quality   which stack of scale steps to voice
// params[1] voicing   how far apart the voices sit
// params[2] inversion how many of the lowest voices go up an octave
// params[3] octave    where a self-playing chord sits: its root is
//                     12 x octave + the key's root, and 0 is the key's own
//                     register, so a chord left alone moves with the key.
//                     Ignored while a note inlet is patched.
// params[4] velocity  what a self-playing chord is sounded at. Ignored
//                     while a note inlet is patched: a played note keeps
//                     the velocity it was played with.
// params[5] retrigger re-strike the chord on every root note-on, including
//                     one that names the note already sounding.
class Chord : public Node{
    public:
        // A ninth is the widest named stack: four steps over the root.
        static constexpr uint8_t MAX_STEPS = 4, MAX_VOICES = MAX_STEPS + 1;
        static constexpr uint16_t P_QUALITY = 0, P_VOICING = 1, P_INVERSION = 2,
                                  P_OCTAVE = 3, P_VELOCITY = 4, P_RETRIGGER = 5;
        static constexpr uint8_t N_PARAMS = 6;

        // Numbered from 1, so that a stored zero is the default triad the way
        // a stored zero is the default everywhere else in this module
        // (node/param.h) and the option names are indexed from the
        // parameter's own minimum.
        enum Quality : uint8_t {
            QUALITY_TRIAD   = 1,
            QUALITY_SEVENTH = 2,
            QUALITY_NINTH   = 3,
            QUALITY_SIXTH   = 4,
            QUALITY_SUS2    = 5,
            QUALITY_SUS4    = 6,
            QUALITY_QUARTAL = 7,
            QUALITY_SHELL   = 8,
            QUALITY_FIFTH   = 9,
            QUALITY_COUNT   = 10,      // one past the last
        };
        enum Voicing : uint8_t {
            VOICING_CLOSE = 0,
            VOICING_OPEN  = 1,
            VOICING_DROP2 = 2,
            VOICING_DROP3 = 3,
            VOICING_WIDE  = 4,
            VOICING_COUNT = 5,
        };
        // A chord of MAX_VOICES has MAX_VOICES - 1 inversions; the parameter
        // stops at the third, which is every inversion a seventh has.
        static constexpr uint8_t MAX_INVERSION = 3;
        // The velocity a note nobody played should sound at.
        static constexpr uint8_t DEFAULT_VELOCITY = 100;
        // No note: what `free_note` holds until the root inlet names one, and
        // what `voiced` holds while nothing is sounding.
        static constexpr uint8_t NO_NOTE = 0xFF;

        static const AlgorithmDescriptor descriptor;
        explicit Chord(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        // A chord the root inlet is playing is held until the next root
        // note-on, so a stopped transport would hold it for ever. A drone
        // nothing is playing, and a chord played through `note in`, are left
        // alone (node/node.h).
        void transport_stopped(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t sounding_count() const { return sounding.count(); }
        // True when this node plays itself: nothing is patched to `note in`.
        bool free_running() const { return in == NO_BUS; }
        // The root a self-playing chord is sounding, or NO_NOTE.
        uint8_t voiced_note() const { return voiced; }
        uint32_t refused() const { return sounding.refused(); }
        // The scale and root actually played: the key's, unless the root
        // inlet has said otherwise.
        uint16_t active_mask() const;
        uint8_t active_root() const;

    private:
        // The quality's voices over `base`, ascending, into `pitch`. Returns
        // how many it wrote, which is always at least one: the root.
        uint8_t build(uint8_t base, uint8_t tonic, uint16_t mask, int16_t* pitch) const;
        // Inverts and then spreads `n` ascending voices in place, leaving
        // them ascending. Pitches may leave 0..127; emit_chord drops those.
        void shape(int16_t* pitch, uint8_t n) const;
        // Sounds every voice, all recorded against `source` so one release
        // takes the whole chord down.
        void emit_chord(BusManager& bus, uint8_t source, uint8_t base, uint8_t tonic,
                        uint16_t mask, uint8_t velocity_out, uint8_t channel);
        // One pass of a chord with no note inlet: works out what it should be
        // playing and re-voices only if that has moved.
        void play_free(BusManager& bus, uint16_t mask, uint8_t tonic);

        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        uint8_t quality;
        uint8_t voicing;
        uint8_t inversion;
        // The pitch class the root inlet last wrote, or NO_NOTE until it
        // has: an unplayed cable has said nothing, so the key still stands.
        uint8_t root;
        uint8_t octave;
        uint8_t velocity;
        bool retrigger;
        // Free-running state. `free_note` is the note the root inlet last
        // named (NO_NOTE: derive it from the key and the octave), `voiced`
        // the root actually sounding, `voiced_mask` the scale it was voiced
        // in, `dirty` a parameter edit that has to be heard, and `stopped`
        // a transport stop this node stood down for: nothing sounds again
        // until the root inlet plays it, so a parameter edit cannot start a
        // chord the stop took down.
        uint8_t free_note;
        uint8_t voiced;
        uint8_t free_channel;
        uint16_t voiced_mask;
        bool dirty;
        bool stopped;
        SoundingNotes sounding;
};

#endif
