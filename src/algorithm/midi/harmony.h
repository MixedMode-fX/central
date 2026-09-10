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
// **The walk is functional, not uniform.** Tonal music does not move at
// random between degrees: it falls by fifths, it approaches the tonic through
// the dominant, it substitutes vi for I. So each style is a 7 x 7 table of
// weights - the same first-order model the harmonic-generation literature
// uses, at a size that fits in flash - and the five differ in what they think
// a chord wants to do next:
//
//   pop     I V vi IV and the deceptive cadence. Strong dominant pull.
//   modal   plagal: I and IV and the degree below the tonic, few leading
//           tones. What a drone wants under it.
//   jazz    down a fifth, over and over: ii-V-I chains around the circle.
//   walk    every degree equally. The null model, for hearing what the
//           others are actually doing.
//   pedal   almost always the tonic, occasionally not. For a patch that
//           should breathe rather than move.
//
// **`gravity` is how hard the tonic pulls.** At 0 the style's table is played
// as written; above it an increasing weight on the tonic is mixed in, and at
// 100 the walk never leaves home whatever the style says. One knob from "the
// style, at full strength" to "a drone", continuous, and worth modulating.
//
// It is named for the pull and not for the movement so that **zero is the
// musical default**: a stored 0 means the descriptor's default everywhere in
// this module (node/param.h), so a parameter whose useful default is "leave
// the style alone" has to *be* zero there, or the setting at the other end
// of it could never be saved. `cadence` pays a smaller version of the same
// price - a stored 0 is 75, and "no cadence at all" is 1, which over any
// phrase count anyone would use is the same thing.
//
// **`phrase` and `cadence` are what make it composed rather than drifting.**
// A walk with no phrase structure wanders; a phrase of four with a cadence of
// 75% resolves to the tonic three times in four, which is a period. And
// `loop` is the difference between improvising and writing: turn it on and
// the next `phrase` chords become the piece, repeated exactly, until it is
// turned off again.
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
// params[0] style     pop / modal / jazz / walk / pedal
// params[1] phrase    chords per phrase
// params[2] cadence   percent chance the phrase's last chord is the tonic
// params[3] gravity   percent pull to the tonic; 100 never leaves it
// params[4] loop      keep the first phrase and repeat it
// params[5] root      the pitch degree 0 sits on
// params[6] scale     0 follows the module's key
// params[7] velocity
// params[8] channel
// params[9] seed      0 draws from the entropy pool, anything else is exact
class Harmony : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Style : uint8_t {
            HARM_POP   = 1,
            HARM_MODAL = 2,
            HARM_JAZZ  = 3,
            HARM_WALK  = 4,
            HARM_PEDAL = 5,
            HARM_STYLES = 5,
        };

        static constexpr uint8_t DEGREES = 7;        // functional degrees the tables describe
        static constexpr uint8_t MAX_PHRASE = 16;
        // What `gravity` mixes in. Sized against the tables' own weights, so
        // that at gravity 100 it is the only non-zero weight there is.
        static constexpr uint8_t TONIC_PULL = 16;

        explicit Harmony(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
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
        uint8_t style;
        uint8_t phrase;
        uint8_t cadence;
        uint8_t gravity;
        uint8_t loop;
        uint8_t root;
        uint8_t scale;
        uint8_t velocity;
        uint8_t channel;
        uint8_t seed;
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
