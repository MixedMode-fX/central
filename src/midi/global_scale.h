#ifndef MMMC_MIDI_GLOBAL_SCALE_H
#define MMMC_MIDI_GLOBAL_SCALE_H

#include <stdint.h>
#include "midi/scale.h"

// The key the module is in: one scale and one root, for the whole patch.
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
    void set(uint8_t scale_id, uint8_t root_pitch_class);

    uint8_t id();                 // the ScaleId, never SCALE_GLOBAL
    uint8_t root();               // pitch class, 0..11
    uint16_t mask();              // the 12-bit mask, never 0

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
}

#endif
