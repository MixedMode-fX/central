#include "patch/default_patch.h"

Patch default_patch(){
    return empty_patch();
}

GlobalSettings default_globals_for_patch(){
    GlobalSettings g = default_globals();
    g.clock_source = 0;                   // MasterClock::CLOCK_INTERNAL
    g.bpm = CLOCK_DEFAULT_BPM;
    // Program Change recall stays off until a user asks for it: a Program
    // Change meant for a downstream synth must not silently switch the patch
    // of a module nobody has configured (#11).
    g.pc_enabled = 0;
    return g;
}
