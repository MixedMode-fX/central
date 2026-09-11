#ifndef MMMC_ALGORITHM_MIDI_HARMONY_H
#define MMMC_ALGORITHM_MIDI_HARMONY_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"

// A chord progression: a walk over the degrees of the key, one chord per
// advance edge.
//
// The module had a key and nothing that ever moved inside it. GlobalSettings
// carries one scale and one root, `Chord` voices a triad, the note sequencers
// pick degrees - and a patch left running plays one chord until somebody
// stops it, which is the most reliable way to make twenty minutes of
// generative music boring. The README already named the patch that was
// missing: "a sequenced root walking through the chords of one key ... is a
// chord progression". This is the node that walks it.
//
// **It only has to emit a root, and that is the whole trick.** `Chord`'s
// intervals are steps of the scale, so 0 2 4 is major on I, minor on ii and
// diminished on vii - the quality of each chord is already correct by
// construction. A node that knows nothing whatever about chord quality still
// produces a diatonic progression, because the module decided years ago that
// an interval is a scale step and not a semitone.
//
// **The walk is computed, not tabulated.** Tonal music does not move at
// random between degrees, and the first version of this node said so with
// five 7 x 7 tables of weights named after genres. Tables are honest about
// what tonal music does and dishonest about everything else: they are written
// for seven degrees, so a pentatonic key used five columns tuned for diatonic
// function and a key nobody anticipated got numbers that meant nothing; they
// say what a genre does rather than why; and the only progressions reachable
// are the ones somebody typed. **There is no room in a table for an
// accident.**
//
// So the weight of every move is derived from the scale instead
// (midi/root_motion.h), out of four facts that hold in any key, each with one
// control over it:
//
//   fifths   how far the root moved and which way round the circle. A root
//            falling a fifth drives tonal music forward; one rising a fifth
//            is the retrograde, and is how a dominant gets approached rather
//            than resolved. At 100 the walk falls by fifths - the circle, and
//            ii-V-I with it - at 1 it rises by them, and at 50 it does not
//            care. Measured in *semitones*, so a fifth is a fifth in a key
//            this control has never seen.
//   smooth   which of two theories of motion is being used. At 0 a move is
//            worth what its interval is worth and the fifths lead; at 100 it
//            is worth what the two chords share - triads a third apart share
//            two notes, a fifth apart one, a step apart none - and the
//            mediants lead, which is what Romantic harmony sounds like.
//            A crossfade rather than a bonus, because a fifth outweighs a
//            third three to one and no amount of added weight turns that
//            over.
//   leading  how much the triad carrying the semitone below the tonic is
//            wanted. That note is what an authentic cadence is made of and
//            what modal music must avoid, so this one control reads as "how
//            tonal" upward and "how modal" downward - and it is inert by
//            itself in a mode that has no leading tone, because there is then
//            no such triad to weight.
//   spread   the one that makes accidents. Below 50 the weights are sharpened
//            - the likeliest move gets likelier and the walk hardens toward a
//            loop. Above 50 they flatten toward every legal move being
//            equally likely. At 50 they are played as computed. Nothing is
//            ever weighted to zero, so the tritone root motion that turns up
//            once in a hundred bars is always available and always in key.
//
// The five old styles are five points in that space and everything between
// them is now reachable, which is where the accidents live. `spread` at 100
// is the uniform walk; `fifths` at 100 with `spread` low is the circle;
// `leading` at 1 is the modal shuttle; `gravity` at 100 is the drone.
//
// **`gravity` is how hard the tonic pulls.** At 0 the computed weights are
// played as they are; above it an increasing weight on the tonic is mixed in,
// and at 100 the walk never leaves home whatever else is set. One knob from
// "the walk, at full strength" to "a drone", continuous, and worth
// modulating.
//
// It is named for the pull and not for the movement so that **zero is the
// musical default**: a stored 0 means the descriptor's default everywhere in
// this module (node/param.h), so a parameter whose useful default is "leave
// the walk alone" has to *be* zero there, or the setting at the other end of
// it could never be saved. `cadence`, `fifths`, `leading` and `spread` pay a
// smaller version of the same price - a stored 0 is their useful middle, and
// 1 is the far end - which over any phrase count anyone would use is the same
// thing as reaching it.
//
// **`phrase` and `cadence` are what make it composed rather than drifting.**
// A walk with no phrase structure wanders; a phrase of four with a cadence of
// 75% resolves to the tonic three times in four, which is a period. And
// `loop` is the difference between improvising and writing: turn it on and
// the next `phrase` chords become the piece, repeated exactly, until it is
// turned off again.
//
// **`drift` is what keeps a loop alive.** An accident that happens once is a
// glitch and one that comes back is a decision, so a looping phrase redraws
// one of its chords with this probability and *keeps* the new one. At 0 the
// loop is exact, which is what it always was. A few percent is a piece that
// is recognisably itself and never quite the same twice, which is the whole
// of what this node is for.
//
// **Degrees the key does not have are not reachable.** The walk runs over the
// first seven degrees of the scale, or over all of them when the scale has
// fewer - a pentatonic key has five chords and the table's last two rows are
// simply never chosen. A chromatic key has twelve degrees and the walk uses
// seven of them, which is chromatic nonsense and exactly what "the module is
// chromatic until a key is set" means: set a key.
//
// Before the first advance nothing is sounding, and the first advance plays
// the tonic. That is what reset means in every sequencer in this module, and
// it means the same here.
//
// Inlet 0 (gate): advance - one chord per rising edge. A Metronome at "1 bar".
// Inlet 1 (gate, optional): reset - the next advance plays the tonic and
//         starts the phrase again.
// Outlet 0 (note): the chord root, held until the next chord.
// Outlet 1 (CV, optional): the degree over full scale, so the modulation
//         matrix can move something else per chord.
//
// params[0] phrase    chords per phrase
// params[1] cadence   percent chance the phrase's last chord is the tonic
// params[2] gravity   percent pull to the tonic; 100 never leaves it
// params[3] loop      keep the first phrase and repeat it
// params[4] root      the pitch degree 0 sits on
// params[5] scale     0 follows the module's key
// params[6] velocity
// params[7] channel
// params[8] seed      0 draws from the entropy pool, anything else is exact
// params[9] fifths    1 rises by fifths, 100 falls by them, 50 neither
// params[10] smooth   which theory of motion: interval, or shared tones
// params[11] leading  1 avoids the leading tone, 100 wants it
// params[12] spread   below 50 sharpens toward a loop, above it flattens
//                     toward a uniform walk
// params[13] drift    percent chance a looping phrase redraws one chord and
//                     keeps it
class Harmony : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        static constexpr uint16_t P_PHRASE = 0, P_CADENCE = 1, P_GRAVITY = 2, P_LOOP = 3,
                                  P_ROOT = 4, P_SCALE = 5, P_VELOCITY = 6, P_CHANNEL = 7,
                                  P_SEED = 8, P_FIFTHS = 9, P_SMOOTH = 10, P_LEADING = 11,
                                  P_SPREAD = 12, P_DRIFT = 13;
        static constexpr uint8_t N_PARAMS = 14;

        static constexpr uint8_t DEGREES = 7;        // functional degrees a triad stack means
        static constexpr uint8_t MAX_PHRASE = 16;
        // Weights are normalised to this before `spread` and `gravity` shape
        // them, so both controls mean the same thing whatever the scale made
        // of the raw numbers - and so the cubing `spread` does at the sharp
        // end cannot overflow.
        static constexpr uint32_t WEIGHT_SCALE = 1000;
        // Defaults for the four controls that describe the walk. Middles, not
        // ends: an unconfigured node should sound like music and leave room
        // in both directions.
        static constexpr uint8_t DEFAULT_FIFTHS = 65, DEFAULT_LEADING = 50, DEFAULT_SPREAD = 35;

        explicit Harmony(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        // The chord is held until the next advance, so a stopped transport
        // would hold it for ever (node/node.h).
        void transport_stopped(BusManager& bus) override { silence(bus); }
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        // The degree being played, 0 = the tonic. 0xFF before the first advance.
        uint8_t degree() const { return started ? current : (uint8_t)0xFF; }
        // The pitch that is sounding, or 0xFF when nothing is.
        uint8_t playing() const { return sounding.count() ? sounding.at(0).note : (uint8_t)0xFF; }
        // Where in the phrase the next chord falls.
        uint8_t phrase_position() const { return position; }
        // How many chords of the phrase have been committed to the loop.
        uint8_t recorded_chords() const { return recorded; }
        // The pitch a degree is played at, after the key is resolved.
        uint8_t pitch_of(uint8_t deg) const;
        // Degrees the key actually has, capped at DEGREES.
        uint8_t usable_degrees() const;
        // The move this walk most wants to make from `from`, before `spread`
        // and the draw. What proves the circle of fifths is computed rather
        // than written down.
        uint8_t likeliest_from(uint8_t from) const;
        // The shaped weight of every degree as seen from `from`, into
        // `weight` (DEGREES entries). Returns how many degrees the key has.
        uint8_t weigh(uint8_t from, uint32_t* weight) const;

    private:
        // The next degree, from the table, the tonic pull and the cadence.
        uint8_t choose();
        // Plays `deg`, releasing the chord that was there.
        void strike(BusManager& bus, uint8_t deg);
        void restart();

        EdgeIn advance_in;
        EdgeIn reset_in;
        uint8_t note_out;
        uint8_t cv_out;
        uint8_t phrase;
        uint8_t cadence;
        uint8_t gravity;
        uint8_t loop;
        uint8_t root;
        uint8_t scale;
        uint8_t velocity;
        uint8_t channel;
        uint8_t seed;
        uint8_t fifths;
        uint8_t smooth;
        uint8_t leading;
        uint8_t spread;
        uint8_t drift;
        uint8_t current;              // the degree being played
        uint8_t position;             // where in the phrase the next chord falls
        uint8_t recorded;             // chords committed to the loop
        bool started;                 // a chord has been played at all
        bool at_first;                // the next advance plays the tonic
        uint8_t written[MAX_PHRASE];  // the phrase, once `loop` is on
        Xorshift32 rng;
        SoundingNotes sounding;
};

#endif
