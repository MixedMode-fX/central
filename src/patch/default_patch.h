#ifndef MMMC_PATCH_DEFAULT_PATCH_H
#define MMMC_PATCH_DEFAULT_PATCH_H

#include "node/patch.h"
#include "patch/patch_codec.h"

// The patch a module boots into when the store is empty or unreadable (#7).
//
// It lives in flash, not in EEPROM, so a freshly flashed module with nothing
// stored still does something observable. That matters more here than on most
// modules: there is no display to explain a silence, and with no panel input
// at all a user who saw nothing happen would have no way to tell a dead
// module from an unconfigured one.
//
// What it does, deliberately visible and audible:
//
//   * **MIDI thru.** Every transport in, every transport out, omni. Plug a
//     keyboard into either DIN port or USB and it plays whatever is
//     downstream, immediately.
//   * **A metronome on jack 1.** The internal clock divided to one pulse per
//     beat, so a scope, an LED or an envelope generator on jack 1 shows the
//     module is alive and running at CLOCK_DEFAULT_BPM.
//   * **A sustain pedal on jack 8.** The one jack whose default direction
//     cannot be guessed wrong: an unpatched input reads as no gate.
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
