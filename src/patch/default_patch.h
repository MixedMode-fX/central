#ifndef MMMC_PATCH_DEFAULT_PATCH_H
#define MMMC_PATCH_DEFAULT_PATCH_H

#include "node/patch.h"
#include "patch/patch_codec.h"

// The patch a module boots into when the store is empty or unreadable (#7).
//
// **It is empty: no jack in use, no MIDI port in use, no node, nothing
// patched.** A module that boots playing something has made three decisions
// on the user's behalf - which jacks face which way, which cables are thru'd,
// what is on note bus 0 - and every one of them is a wire they have to find
// and undo before their own patch means what it says. Silence is the only
// honest starting point, and it is the same one every block starts from: a
// node arrives on no bus and is connected by being dragged.
//
// It is also what "restore defaults" restores. That is a host-side command -
// over SysEx (#11) or the console - because there is no button to hold at
// power-on.
Patch default_patch();

// The globals that go with it: the internal clock at the default tempo, and
// Program Change recall off, so a Program Change meant for a downstream synth
// cannot silently switch a patch on a module nobody has configured (#11).
GlobalSettings default_globals_for_patch();

#endif
