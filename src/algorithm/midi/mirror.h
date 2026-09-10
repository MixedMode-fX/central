#ifndef MMMC_ALGORITHM_MIDI_MIRROR_H
#define MMMC_ALGORITHM_MIDI_MIRROR_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "util/random.h"

// Reflects notes about an axis in the key: negative harmony, and its simpler
// relative, plain inversion.
//
// **Negative harmony is the circle of fifths reflected.** Take the axis
// halfway between the tonic and the dominant - between E flat and E in C -
// and reflect every pitch class through it. Relative to the tonic that is
// `7 - pitch class`, and the whole theory is that one expression:
//
//     C E G   (I)   ->  C E flat G      i
//     G B D   (V)   ->  F A flat C      iv
//     F A C   (IV)  ->  G B flat D      v
//
// The dominant becomes the subdominant minor and the subdominant becomes the
// minor dominant, because the reflection exchanges the two halves of the
// circle about the tonic. A progression put through it comes back as its own
// shadow: a different piece of music, derived for nothing from one that
// already works, which is the ideal generative move.
//
// `inversion` is the same operation about the tonic itself rather than about
// the tonic-dominant axis - `-pitch class` instead of `7 - pitch class` - and
// is what a melody wants rather than what a progression does.
//
// **It reflects the pitch class and then re-registers.** Reflecting the pitch
// is the obvious reading and the wrong one: middle C about C's axis is `7 -
// 60`, which is not a note. So the reflection is taken modulo twelve and the
// result placed in the octave nearest the note that caused it, which is also
// what makes `amount` usable - at fifty percent the reflected notes sit among
// the ones that passed straight through instead of two octaves underneath
// them.
//
// The axis is the key's: the module's scale and root unless this node names
// its own, and a patched root inlet outranks both - the same rule
// `NoteQuantise` follows, for the same reason. `snap` puts the reflection
// back in the scale afterwards; it is off by default, because a reflection
// that stayed in the key would be a transposition, and leaving the key is the
// point.
//
// **`amount` is a percentage and cannot be zero**, for the reason
// `Harmony::cadence` cannot: a stored 0 means the descriptor's default
// everywhere in this module, so the default has to *be* zero there or it
// could never be saved. The default is 100 - every note reflected, which is
// what somebody who patched a mirror asked for - and 1% is what "off" is
// spelled as.
//
// A note that reflects outside 0..127 is dropped, like Transpose and for the
// same reason, and so is its note-off by construction: nothing was recorded.
//
// Inlet 0 (note): the notes to reflect.
// Inlet 1 (note, optional): the axis root. Note-ons set it; nothing else is
//         read.
// Outlet 0 (note): the reflection.
//
// params[0] mode     negative / inversion
// params[1] scale    0 follows the module's key
// params[2] root     the pitch class the axis is built on, when this node
//                    names its own scale and no root inlet is patched
// params[3] amount   percent of note-ons reflected; the rest pass through
// params[4] snap     put the reflection back in the scale
// params[5] seed     0 draws from the entropy pool, anything else is exact
class Mirror : public Node{
    public:
        static constexpr uint16_t P_MODE = 0, P_SCALE = 1, P_ROOT = 2, P_AMOUNT = 3,
                                  P_SNAP = 4, P_SEED = 5;
        static constexpr uint8_t N_PARAMS = 6;
        static constexpr uint8_t DEFAULT_AMOUNT = 100;

        enum Mode : uint8_t {
            MIRROR_NEGATIVE  = 1,   // about the axis between the tonic and the dominant
            MIRROR_INVERSION = 2,   // about the tonic
            MIRROR_MODES     = 2,
        };

        static const AlgorithmDescriptor descriptor;
        explicit Mirror(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }
        uint16_t active_mask() const;
        uint8_t active_root() const;
        // What `note` becomes: the reflection, in the octave nearest it, and
        // snapped if this node snaps. 0xFF when it would leave 0..127.
        uint8_t reflect(uint8_t note) const;

    private:
        uint8_t in;
        uint8_t root_in;
        uint8_t out;
        uint8_t mode;
        uint8_t scale;
        uint8_t root;
        uint8_t amount;
        bool snap;
        Xorshift32 rng;
        SoundingNotes sounding;
};

#endif
