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
// setting lives here instead, and an algorithm's own scale and key
// parameters become *overrides* - stored as SCALE_GLOBAL / an empty mask and
// as KEY_FOLLOW by default (midi/scale.h), which is what a zeroed preset byte
// already was, so the default is "whatever key the module is in" and a node
// has to name a scale, or a root, to leave it. The two are separate
// questions and separate parameters: see KeyFollow below.
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
    inline bool follows(uint8_t scale_id){ return scale_mask(scale_id) == 0; }

    // **Which notes and which of them is home are two questions.**
    //
    // They used to be one: a node that named a scale of its own kept its own
    // root, and one that followed the module's scale followed its root. That
    // reads well until a patch wants what patches actually want - the whole
    // module in A minor, with a quantiser snapping to the pentatonic and a
    // chord voicing the harmonic minor - and every node that names a mode of
    // its own has silently left the key as well, playing that mode on C.
    //
    // So each node carries a `key` parameter beside its `scale` one, and it
    // is this enum. `follow` is zero, so the default is the key and a node
    // that wants to sit somewhere else has to say so - the same convention
    // the scale parameter follows, applied to the other half of a key.
    enum KeyFollow : uint8_t { KEY_FOLLOW = 0, KEY_OWN = 1, KEY_MODES = 2 };

    // **The register a node asks for, in octaves from where it sits by
    // default.**
    //
    // The key names one register and a patch needs several: a bass line, a
    // pad and a lead are the same key in three different octaves, so a key
    // with a register that moved every node to the same note would be a key
    // nobody could use. A node's own root parameter answers it - not as a
    // pitch, which would fight the key's root, but as a distance from the
    // pitch that parameter holds when nothing has touched it. Left alone it
    // is zero, so a patch that has not been moved lands on the key's root
    // note and one setting still moves the whole patch.
    inline int8_t register_offset(uint8_t root_pitch, uint8_t default_pitch){
        return (int8_t)((int16_t)(root_pitch / 12u) - (int16_t)(default_pitch / 12u));
    }

    // The pitch class a node plays in: the key's, unless the node names its
    // own. For a node whose root parameter is a pitch class - a quantiser, a
    // delay, the axis of a mirror - and for the key half of any node.
    inline uint8_t resolve_root(uint8_t key, uint8_t node_root){
        return key == KEY_FOLLOW ? root() : (uint8_t)(node_root % 12u);
    }

    // The tonic for a node whose root parameter names a **pitch** rather than
    // a pitch class: a harmony deciding where its chords sit, a quantiser
    // deciding the bottom of its range, a CV input deciding what its degrees
    // are measured from.
    //
    // Following the key, it is the key's root note shifted by the register
    // this node asks for; with no register set there is nothing to shift
    // from, so it is the key's pitch class inside the octave the parameter
    // names - which is what the module did before a register existed.
    // `default_pitch` is that parameter's descriptor default, and is what
    // makes "where it sits by default" a number rather than a guess.
    //
    // Folded rather than clipped: a tonic past either end of the keyboard is
    // the same pitch class an octave the other way.
    inline uint8_t resolve_tonic(uint8_t key, uint8_t root_pitch, uint8_t default_pitch){
        if (key != KEY_FOLLOW) return root_pitch;
        const uint8_t home = root_note();
        int16_t tonic = (home != NO_ROOT_NOTE)
            ? (int16_t)home + (int16_t)register_offset(root_pitch, default_pitch) * 12
            : (int16_t)(root_pitch - (root_pitch % 12u)) + (int16_t)root();
        while (tonic > 127) tonic -= 12;
        while (tonic < 0) tonic += 12;
        return (uint8_t)tonic;
    }

    // The anchor for a node whose root is the pitch a **stored pattern** was
    // written around - the note sequencers, and a chord playing itself.
    //
    // The one case where a key with no register cannot help: a pattern of
    // degrees measured from C3 is not the same pattern measured from A, and
    // re-rooting it on a pitch class would have to guess a register. So with
    // no register set the pattern keeps its own anchor exactly as it always
    // has; with one, it is the key's root note, moved by the register this
    // node asks for.
    //
    // A patched root inlet outranks this, as it outranks everything.
    inline uint8_t resolve_anchor(uint8_t key, uint8_t root_pitch, uint8_t default_pitch){
        if (key != KEY_FOLLOW) return root_pitch;
        const uint8_t home = root_note();
        if (home == NO_ROOT_NOTE) return root_pitch;
        int16_t anchor = (int16_t)home + (int16_t)register_offset(root_pitch, default_pitch) * 12;
        while (anchor > 127) anchor -= 12;
        while (anchor < 0) anchor += 12;
        return (uint8_t)anchor;
    }
}

#endif
