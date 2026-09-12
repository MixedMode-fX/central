#ifndef MMMC_MIDI_GLOBAL_KEY_H
#define MMMC_MIDI_GLOBAL_KEY_H

#include <stdint.h>
#include "midi/scale.h"

// The key the module is in: one scale, one root and one register, for the
// whole patch. There is no second copy of it anywhere.
//
// Every algorithm that has a scale used to keep its own, which is right for
// the algorithm and wrong for the instrument: changing key meant editing a
// quantiser, two sequencers and a chord voicer and hoping they agreed. The
// obvious repair is a global setting each node may override, and that is
// what this was - a `scale` parameter, a `key` parameter and a `root`
// parameter on eight algorithms, three ways for a node to leave the key it
// is in. Nobody wants a patch in two keys at once. The parameters are gone
// and this is the only key there is.
//
// **What a node still chooses is the register**, because a bass line, a pad
// and a lead are the same key in three different octaves. A node with an
// absolute root carries one `octave` parameter and nothing else: zero, its
// default, is the key's own register, so one setting moves the whole patch;
// 1..10 names an octave outright and stays there whatever the key does.
//
// It is a global because it is genuinely one value for the module and
// because a node's process() is handed nothing but the buses: threading a
// key through BusManager would make it a signal, which it is not - it is
// control-plane state, set between passes and read during them. It travels
// in GlobalSettings (patch/patch_codec.h), so it is part of a preset and of
// the patch dump.
//
// **Two writers, and they mean different things.** PatchManager::push_globals
// writes the key a patch was saved with. The Key node (algorithm/midi/key.h)
// writes the key a performance has moved to, from a note bus, and never
// touches the stored settings - the same rule a modulated parameter follows
// (PatchManager::modulate_param says why at length). A CC, an NRPN or a
// modulation route reaches it as CC_TARGET_KEY, through the one applier every
// other control-plane write goes through.
//
// Chromatic on C until a user sets a key, which is what a zeroed preset byte
// already gave.
namespace global_key {
    // The register a zeroed `octave` parameter, and a zeroed preset byte,
    // land on: 5 * 12 is 60, so the key's root note is middle C by default.
    static constexpr uint8_t DEFAULT_OCTAVE = 5;
    // The highest octave whose root can be a note at all: 10 * 12 is 120, and
    // above that a pitch class of 8 or more has nowhere to sit.
    static constexpr uint8_t MAX_OCTAVE = 10;

    // From the patch's GlobalSettings. A zero scale byte, a root past B and
    // a zero octave are all read as the default rather than refused, because
    // this is also what an all-zero preset means.
    void set(uint8_t scale_id, uint8_t root_pitch_class, uint8_t register_octave = 0);
    // The root alone, for a performance that moves the key without touching
    // the scale it is in - the Key node's note inlet, and a CC bound to
    // CC_KEY_ROOT.
    void set_root(uint8_t root_pitch_class);
    void set_scale(uint8_t scale_id);
    void set_octave(uint8_t register_octave);

    uint8_t id();       // the ScaleId, never SCALE_NONE
    uint8_t root();     // pitch class, 0..11
    uint16_t mask();    // the 12-bit mask, never 0
    uint8_t octave();   // the key's register, 1..MAX_OCTAVE

    // **The tonic a node plays from, as a pitch rather than a pitch class.**
    //
    // A scale and a pitch class say which notes and which of them is home;
    // they cannot say *where* home is, and every node with an absolute root -
    // a harmony deciding where its chords sit, a quantiser deciding the
    // bottom of its range, a sequencer deciding what pitch its degrees are
    // measured from - needs that.
    //
    // `node_octave` is the node's own `octave` parameter: 0 for the key's
    // register, 1..MAX_OCTAVE for one of its own. Folded rather than clipped,
    // so a tonic past the top of the keyboard is the same pitch class an
    // octave down instead of a different note.
    uint8_t tonic(uint8_t node_octave);
}

#endif
