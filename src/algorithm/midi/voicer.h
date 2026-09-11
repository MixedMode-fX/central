#ifndef MMMC_ALGORITHM_MIDI_VOICER_H
#define MMMC_ALGORITHM_MIDI_VOICER_H

#include "node/node.h"
#include "midi/held_notes.h"
#include "midi/sounding_notes.h"

// Places a chord's notes: where they sit, and what moves when it changes.
//
// `Harmony` chooses the roots and `Chord` stacks the right notes on them, and
// between the two of them a progression is still block chords in root
// position - every voice jumping the same distance as the root, every chord
// struck from nothing. That is the layer this node is: the chord arrives, and
// what leaves is the same chord placed so that the voices move as little as
// they can.
//
// **The notes two chords share are held, not re-struck.** Two triads a fifth
// apart share one tone and two a third apart share two, so on the fifth-walk
// `Harmony` is best at, this is the difference between a chord change and a
// chord *moving*. It is also the reason this node's ledger is keyed on the
// note it emitted rather than on the note that caused it, which every other
// modifier in the tree does the other way round: a modifier that emits a
// function of one note must release exactly what that note sent, but this one
// emits a function of the whole held set, and the question it answers on
// every change is "is this pitch already sounding" - a question about what
// was emitted. Key it that way and common-tone retention is not code, it is
// the absence of code: the shared notes are neither released nor emitted,
// because nothing asks them to be.
//
// **A chord is what is held at the end of a pass, not what arrived during
// it.** `Chord` re-voices by releasing everything it had and sounding the new
// chord, so a chord change reaches this node as a handful of note-offs and a
// handful of note-ons in one pass, in that order. Reading events one at a
// time would voice the empty chord in between; collecting them into the held
// set and voicing once at the end of the pass sees one chord replace another,
// which is what happened.
//
// **`bass` and minimal motion disagree, and the disagreement is musical.**
// Voice-lead C E G to F A C with the bass free and C stays where it is: three
// semitones of motion in total. Pin the lowest voice to the chord's own
// bass - so the root motion is audible, which for a progression is usually
// the point - and it is fifteen. Both are wanted, neither is a compromise of
// the other, so it is a switch and this is what it costs.
//
// Modes:
//   closest  every voice takes the nearest pitch to where the voices were.
//            The first chord has nothing to be near, so it is stacked up from
//            `low`.
//   root     stacked up from `low` every time, ignoring what came before. The
//            null setting: what the chord did before this node existed.
//   drop 2   root position, then the second voice from the top dropped an
//            octave. The standard open voicing, and it is deliberately not
//            voice-led: a fixed shape is what it is for.
//   spread   stacked up from `low` with at least a fifth between voices, so a
//            triad covers three octaves.
//
// Notes that are not note-ons or note-offs pass straight through, as they do
// in every modifier here.
//
// Inlet 0 (note): the chord to place.
// Outlet 0 (note): the same chord, placed.
//
// params[0] mode      closest / root / drop 2 / spread
// params[1] low       the bottom of the range voices are placed in
// params[2] high      the top of it; forced to at least an octave above `low`
// params[3] voices    how many of the chord's pitch classes to keep (0: all)
// params[4] bass      keep the chord's own bass as the lowest voice
// params[5] retrigger re-strike every voice on every change, common tones
//                     included
class Voicer : public Node{
    public:
        static constexpr uint8_t MAX_VOICES = 8;
        static constexpr uint16_t P_MODE = 0, P_LOW = 1, P_HIGH = 2, P_VOICES = 3,
                                  P_BASS = 4, P_RETRIGGER = 5;
        static constexpr uint8_t N_PARAMS = 6;
        static constexpr uint8_t DEFAULT_LOW = 48, DEFAULT_HIGH = 84;
        // The gap `spread` forces between adjacent voices: a fifth, which is
        // the widest one that still leaves a triad inside three octaves.
        static constexpr uint8_t SPREAD_GAP = 7;

        enum Mode : uint8_t {
            VOICE_CLOSEST = 1,
            VOICE_ROOT    = 2,
            VOICE_DROP2   = 3,
            VOICE_SPREAD  = 4,
            VOICE_MODES   = 4,
        };

        static const AlgorithmDescriptor descriptor;
        explicit Voicer(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        uint8_t voice_count() const { return n_voiced; }
        // The voicing being sounded, ascending. Out of range reads as 0.
        uint8_t voice(uint8_t index) const { return index < n_voiced ? voiced[index] : (uint8_t)0; }
        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }
        // The range actually used: `high` is forced to an octave above `low`,
        // because a range narrower than one cannot hold every pitch class.
        uint8_t range_low() const;
        uint8_t range_high() const;

    private:
        // The pitch of class `pc` nearest to `anchor`, brought inside the
        // range. Never leaves 0..127.
        uint8_t place(uint8_t pc, uint8_t anchor) const;
        // The lowest pitch of class `pc` at or above `floor_note`.
        uint8_t stack(uint8_t pc, uint8_t floor_note) const;
        // Works out where the held chord should sit, into `want`, ascending.
        // Returns how many voices it wrote.
        uint8_t voicing(uint8_t* want) const;
        // Releases what is no longer wanted, sounds what is new, and leaves
        // the notes in both alone.
        void revoice(BusManager& bus);

        uint8_t in;
        uint8_t out;
        uint8_t mode;
        uint8_t low;
        uint8_t high;
        uint8_t voices;
        bool bass;
        bool retrigger;
        // The chord as it arrived, and the velocity and channel to sound it
        // at: whatever the note-on that last changed it was played with.
        HeldNotes held;
        uint8_t velocity;
        uint8_t channel;
        // The voicing currently sounding, ascending. This is what "nearest to
        // where the voices were" is measured against.
        uint8_t voiced[MAX_VOICES];
        uint8_t n_voiced;
        bool dirty;
        SoundingNotes sounding;
};

#endif
