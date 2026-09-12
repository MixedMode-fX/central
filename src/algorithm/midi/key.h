#ifndef MMMC_ALGORITHM_MIDI_KEY_H
#define MMMC_ALGORITHM_MIDI_KEY_H

#include "node/node.h"

// The key, in the patch: a note bus plays it.
//
// The key the module is in is one setting for the whole patch and lives in
// GlobalSettings (midi/global_key.h). An editor sets it, a knob sets it - a
// CC, an NRPN or a modulation route bound to CC_TARGET_KEY, none of which
// needs a node - and that covers every way of changing a key *from outside*
// the patch. This is the one from inside it: a note bus reaching the key, so
// the module can modulate itself.
//
// That is a different instrument. A Harmony walking the degrees of one key
// is a progression; a sequencer moving the *key* under it every eight bars
// is a piece with sections, and there was no way to build the second. Patch a
// slow sequencer, a Turing machine or a keyboard into `root` and every node
// that plays a note moves with it, at once.
//
// **It holds no key of its own.** Its parameters say how to read a note, not
// what the key is - because a key stored here as well as in the settings
// would be two copies of one value, and the whole point of the change that
// created this node was that there is exactly one. What it writes is the key
// that is *playing*: the settings are left alone, so a preset saved while a
// sequencer is running captures the key the patch was written in rather than
// whichever bar it happened to be on. That is precisely the rule a modulated
// parameter follows, and PatchManager::modulate_param argues it at length.
//
// **One per patch.** The key has one value, and two nodes writing it would be
// two writers racing over it - which the module already refuses for two
// modulation routes on one target. The patch validator refuses a second one
// (MixedModeMaster::validate).
//
// It runs before every node that reads the key, whatever order the patch
// lists them in, so a key change is heard by the notes of the same pass
// rather than a pass later (node/node.h, node/schedule.h).
//
// Inlet 0 (note): the key's root. A note-on sets it; nothing else is read,
//         and a note-off never moves the key - a key is a place the music is,
//         not a note somebody is holding. It is required: a Key node with
//         nothing patched to it moves nothing at all.
//
// params[0] channel  1..16, or 0 for any. A note bus carries channels, so a
//                    progression on one channel can move the key while the
//                    parts on the others do not.
// params[1] from     what a note-on takes from the note: its pitch class
//                    only, or its register as well. Pitch class alone by
//                    default, because a sequencer written across two octaves
//                    would otherwise move the whole patch up and down with
//                    its own melody.
class Key : public Node{
    public:
        static constexpr uint16_t P_CHANNEL = 0, P_FROM = 1;
        static constexpr uint8_t N_PARAMS = 2;

        // Numbered from 1, so a stored zero is the default the way it is
        // everywhere else in this module (node/param.h).
        enum From : uint8_t {
            KEY_FROM_PITCH_CLASS = 1,   // the root only
            KEY_FROM_NOTE        = 2,   // the root and the register it sits in
            KEY_FROMS            = 2,
        };

        static const AlgorithmDescriptor descriptor;
        explicit Key(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests: how many note-ons have moved the key.
        uint32_t moves() const { return move_count; }

    private:
        uint8_t root_in;
        uint8_t channel;
        uint8_t from;
        uint32_t move_count;
};

#endif
