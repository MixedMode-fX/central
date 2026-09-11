#ifndef MMMC_MIDI_GLOBAL_SCALE_H
#define MMMC_MIDI_GLOBAL_SCALE_H

#include <stdint.h>
#include "midi/scale.h"

// The key the module is in: one scale, one root, and optionally the register
// it sits in - for the whole patch.
//
// Every algorithm that has a scale had its own copy of one, which is right
// for the algorithm and wrong for the instrument: changing key meant editing
// a quantiser, two sequencers and a chord voicer and hoping they agreed. The
// setting lives here instead, and an algorithm's own scale parameter becomes
// an *override* - it is stored as SCALE_GLOBAL / an empty mask by default
// (midi/scale.h), which is what a zeroed preset byte already was, so the
// default is "whatever key the module is in" and naming a scale opts out.
//
// It is a global because it is genuinely one value for the module and
// because a node's process() is handed nothing but the buses: threading a
// key through BusManager would make it a signal, which it is not - it is
// control-plane state, set between passes and read during them. It travels
// in GlobalSettings (patch/patch_codec.h), so it is part of a preset and of
// the patch dump, and PatchManager::push_globals() is the one writer.
//
// Chromatic until a user sets a key, so a patch written before this existed
// plays exactly the notes it always did.
namespace global_scale {
    // From the patch's GlobalSettings. `scale_id` is a ScaleId; SCALE_GLOBAL
    // is not one (the global scale cannot follow itself) and is read as
    // chromatic, which is also what a zeroed preset byte gives.
    void set(uint8_t scale_id, uint8_t root_pitch_class, uint8_t root_octave = 0);

    uint8_t id();                 // the ScaleId, never SCALE_GLOBAL
    uint8_t root();               // pitch class, 0..11
    uint16_t mask();              // the 12-bit mask, never 0
    uint8_t octave();             // 1..10, or 0 when the key names no register

    // What `root_note()` returns when the key names no register. Every
    // resolver below falls back to the node's own root on it, which is what
    // the module did before a register existed.
    static constexpr uint8_t NO_ROOT_NOTE = 0xFF;

    // **The key's root as a pitch, not a pitch class.**
    //
    // A scale and a pitch class say which notes and which of them is home;
    // they cannot say *where* home is. So every node with an absolute root -
    // a harmony deciding where its chords sit, a quantiser deciding the
    // bottom of its range, a sequencer deciding what pitch its degrees are
    // measured from - kept a register of its own, and moving the patch an
    // octave meant editing each of them. The note sequencers could not follow
    // the key at all for exactly this reason: their root names an octave, and
    // a pitch class cannot.
    //
    // The register is optional and unset by default, so a patch written
    // before it existed plays precisely the notes it always did. Set it and
    // one setting moves the whole patch.
    //
    // NO_ROOT_NOTE when no register is set; otherwise `octave * 12 + root`,
    // dropped an octave rather than allowed past 127.
    uint8_t root_note();

    // What an algorithm actually plays. `node_mask` is the algorithm's own
    // scale - an empty mask means it did not name one, so it follows the
    // module's.
    inline uint16_t resolve(uint16_t node_mask){
        return (node_mask & 0x0FFF) ? (uint16_t)(node_mask & 0x0FFF) : mask();
    }
    inline uint16_t resolve_id(uint8_t scale_id){ return resolve(scale_mask(scale_id)); }
    // The root that goes with it: an algorithm following the module's scale
    // follows its root too, because a scale without a root is not a key.
    // One that names its own scale keeps its own root.
    inline uint8_t resolve_root(uint8_t scale_id, uint8_t node_root){
        return scale_mask(scale_id) ? (uint8_t)(node_root % 12u) : root();
    }
    inline bool follows(uint8_t scale_id){ return scale_mask(scale_id) == 0; }

    // The tonic for a node whose root parameter names a **pitch** rather than
    // a pitch class.
    //
    // A node that names its own scale keeps its own root entirely, which is
    // resolve_root()'s rule and therefore this one's. One that follows the
    // module takes the key's root note when the key names a register, and
    // otherwise takes the key's *pitch class* inside the octave the parameter
    // names - so a patch set to C3 in A minor plays from A3, and setting the
    // key's register moves every such node at once.
    inline uint8_t resolve_tonic(uint8_t scale_id, uint8_t root_pitch){
        if (scale_mask(scale_id)) return root_pitch;        // its own scale, its own root
        const uint8_t home = root_note();
        if (home != NO_ROOT_NOTE) return home;
        int16_t tonic = (int16_t)(root_pitch - (root_pitch % 12u)) + (int16_t)root();
        if (tonic > 127) tonic -= 12;
        return (uint8_t)tonic;
    }

    // The anchor for a node whose root is the pitch a **stored pattern** was
    // written around - the note sequencers.
    //
    // These were the exception to the key's root half, and the reason was
    // that a pitch class cannot name an octave: a pattern of degrees measured
    // from C3 is not the same pattern measured from A, and re-rooting it on a
    // pitch class would have had to guess a register. A key with a register
    // does not have to guess, so the exception is only for the case that
    // still cannot: with no register set the pattern keeps its own anchor
    // exactly as it always has, and the key's pitch class is not applied to
    // it.
    //
    // A patched root inlet outranks this, as it outranks everything.
    // `node_mask` is the algorithm's own scale, empty when it named none -
    // the note sequencers store a mask rather than an id, as `resolve` does.
    inline uint8_t resolve_anchor(uint16_t node_mask, uint8_t root_pitch){
        if (node_mask & 0x0FFF) return root_pitch;          // its own scale, its own root
        const uint8_t home = root_note();
        return home != NO_ROOT_NOTE ? home : root_pitch;
    }
    inline uint8_t resolve_anchor_id(uint8_t scale_id, uint8_t root_pitch){
        return resolve_anchor(scale_mask(scale_id), root_pitch);
    }
}

#endif
