#ifndef MMMC_ALGORITHM_MIDI_TONNETZ_H
#define MMMC_ALGORITHM_MIDI_TONNETZ_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"

// A triad walking the Tonnetz: chromatic harmony where no voice moves more
// than a tone.
//
// `Harmony` walks the degrees of a key and never leaves it, which is what a
// key is for and also its limit - every chord it can reach is one of seven.
// This is the other walk. Three transforms take a major or minor triad to
// another one, each of them moving exactly one voice by a semitone or a tone
// and leaving the other two where they are:
//
//   P  parallel            C major <-> C minor        the third moves a semitone
//   L  leading-tone        C major <-> E minor        the root moves down a semitone
//   R  relative            C major <-> A minor        the fifth moves up a tone
//
// **The circle of fifths is one of the cycles of this object.** Alternate two
// of the three transforms and the roots trace a symmetric division of the
// octave:
//
//   L then R    fifths                 C  Em G  Bm D  ...
//   P then L    major thirds           C  Cm A flat  A flat m  E  Em  C
//   P then R    minor thirds           C  Cm E flat  E flat m  G flat ...
//
// So this is not a different idea from `Harmony`, it is the same one with the
// other two axes exposed - which is the argument for having both. `LR` stays
// inside a key most of the time and sounds like a progression; `PL` and `PR`
// leave it immediately and sound like film music, because every step is still
// one voice moving one semitone.
//
// **It emits a triad in root position and leaves the voice leading to
// `Voicer`.** Parsimonious voice leading is the entire point of these
// transforms, so a node that emitted them unvoiced looks wrong until the
// voicer exists - and then `Tonnetz -> Voicer` produces the one-voice-moves
// motion by construction, because two triads that share two notes are exactly
// the case a closest voicing holds two notes through. Doing it here would be
// a second voicer, and this is a generator like `Harmony` rather than one.
//
// `deviation` is the chance of taking one of the other two transforms instead
// of the cycle's next. At 0 the cycle is exact and repeats - `PL` returns to
// its starting triad after six steps, `PR` after eight, `LR` after
// twenty-four - and a few percent is what turns a cycle into a walk.
//
// `diatonic` refuses any transform whose triad is not entirely inside the
// key, trying the others in turn and staying put if none fits. `P` is never
// diatonic, so in practice it restricts the walk to `L` and `R` - which is
// the fifth cycle, which is `Harmony`'s territory arrived at from the other
// direction.
//
// Before the first advance nothing is sounding, and the first advance plays
// the starting triad. Reset means the same here as in every sequencer: the
// next advance starts over.
//
// Inlet 0 (gate): advance - one transform per rising edge.
// Inlet 1 (gate, optional): reset - back to the starting triad.
// Outlet 0 (note): the triad, held until the next one.
//
// params[0] cycle      LR / PL / PR / free
// params[1] deviation  percent chance of a transform the cycle did not name
// params[2] diatonic   refuse a triad the key does not contain
// params[3] root       the pitch the starting triad is built on. Follows the
//                      key like every other absolute root here: its pitch
//                      class always, and its register too when the key names
//                      one (midi/global_scale.h).
// params[4] minor      start on a minor triad rather than a major one
// params[5] scale      0 follows the module's key; only `diatonic` reads it
// params[6] velocity
// params[7] channel
// params[8] seed       0 draws from the entropy pool, anything else is exact
class Tonnetz : public Node{
    public:
        static constexpr uint16_t P_CYCLE = 0, P_DEVIATION = 1, P_DIATONIC = 2, P_ROOT = 3,
                                  P_MINOR = 4, P_SCALE = 5, P_VELOCITY = 6, P_CHANNEL = 7,
                                  P_SEED = 8;
        static constexpr uint8_t N_PARAMS = 9;
        static constexpr uint8_t DEFAULT_ROOT = 48, DEFAULT_VELOCITY = 100;

        enum Cycle : uint8_t {
            TONNETZ_LR   = 1,
            TONNETZ_PL   = 2,
            TONNETZ_PR   = 3,
            TONNETZ_FREE = 4,
            TONNETZ_CYCLES = 4,
        };

        enum Transform : uint8_t { TRANSFORM_P = 0, TRANSFORM_L = 1, TRANSFORM_R = 2 };

        static const AlgorithmDescriptor descriptor;
        explicit Tonnetz(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        // The pitch class the triad is rooted on, 0xFF before the first
        // advance.
        uint8_t triad_root() const { return started ? current_root : (uint8_t)0xFF; }
        bool triad_is_minor() const { return current_minor; }
        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }
        // One transform applied to a triad, as a pure function: what makes
        // the three definitions testable without a bus.
        static void apply(uint8_t transform, uint8_t& root_pc, bool& minor);
        // Where the walk starts and which register it sits in, after the
        // module's key has been resolved into `root`.
        uint8_t active_root() const;

    private:
        // The transform the cycle names next, before `deviation` is rolled.
        uint8_t scheduled() const;
        // The transform actually taken: `scheduled`, deviated, and rejected by
        // `diatonic` if it has to be. 0xFF when nothing is allowed.
        uint8_t choose();
        // True when every note of the triad is in the key.
        bool in_key(uint8_t root_pc, bool minor) const;
        void strike(BusManager& bus);
        void restart();

        EdgeIn advance_in;
        EdgeIn reset_in;
        uint8_t note_out;
        uint8_t cycle;
        uint8_t deviation;
        bool diatonic;
        uint8_t root;
        bool minor;
        uint8_t scale;
        uint8_t velocity;
        uint8_t channel;
        uint8_t seed;
        uint8_t current_root;      // pitch class
        bool current_minor;
        uint8_t step;              // which half of the cycle comes next
        bool started;
        bool at_first;
        Xorshift32 rng;
        SoundingNotes sounding;
};

#endif
