#ifndef MMMC_PATCH_PATCH_MANAGER_H
#define MMMC_PATCH_PATCH_MANAGER_H

#include <stdint.h>
#include "master.h"
#include "patch/patch_store.h"
#include "patch/patch_codec.h"
#include "led/status_leds.h"

// The one path a patch takes to become the running graph (#7, #11).
//
// Everything that changes the module's configuration goes through here: the
// console, the SysEx protocol, Program Change recall and the editor. One
// staging buffer, one validator, one swap, one handover - so there is one set
// of tests rather than one per transport, and a bug fixed for one caller is
// fixed for all of them.
//
// **Staging, then validate, then swap.** The live graph is evaluated every
// pass and every clock tick, so mutating it in place would produce partial
// graphs, nodes reading buses nothing writes any more, and notes held by a
// modifier that no longer exists. Instead a caller fills `staging()`, and
// `commit()` validates the whole thing and either swaps it in or changes
// nothing at all. A patch is never partially applied.
//
// **Handover on swap.** MixedModeMaster::unload() already gives every node
// its chance to release what it owns (Node::silence) and flushes the
// note-offs to the transports before anything is destroyed, so a swap under
// a held chord cannot hang a downstream synth. What survives a swap is
// nothing: every node is constructed fresh, so a sequencer restarts at its
// first step and a divider re-phases onto the master count. That is the
// written rule #11 asks for, and it is what "the patch is the whole
// configuration" means - a patch that half-remembered the last one would be
// a patch that plays differently the second time it is recalled.
//
// **A bad patch cannot lock the module out.** Nothing here is a graph node,
// nothing here is reachable from a bus, and the caller that drives it - the
// console, the SysEx handler - runs whatever the loaded patch does. If a
// patch fails to validate the previous one keeps running; if the stored one
// fails at boot the default patch runs and the red LED says so. There is no
// panel button to hold, so there must be no state a patch can put the module
// into that a host cannot get it out of.

enum ApplyError : uint8_t {
    APPLY_OK = 0,
    APPLY_INVALID,          // the validator refused it; see master.last_error()
    APPLY_NO_SUCH_SLOT,
    APPLY_SLOT_EMPTY,
    APPLY_SLOT_CORRUPT,
    APPLY_TOO_LARGE,        // the patch does not fit a preset slot
};

class PatchManager {
    public:
        PatchManager(MixedModeMaster& master, PatchStore& store, StatusLeds& leds);
        PatchManager(const PatchManager&) = delete;
        PatchManager& operator=(const PatchManager&) = delete;

        // Boot (#7): run what was stored in slot 0, or the default patch if
        // there is nothing valid there. A freshly flashed module with an
        // empty EEPROM comes up doing something observable, and a module
        // whose stored patch is corrupt runs the default with the red LED
        // solid rather than running something half-valid.
        void boot(uint32_t now_us);

        // Applies `staging()` after validating it whole. On failure nothing
        // changes and the running patch is untouched.
        ApplyError commit(uint32_t now_us);
        // Copies `patch`/`globals` into staging and commits in one step.
        ApplyError apply(const Patch& patch, const GlobalSettings& globals, uint32_t now_us);
        // Back to the flash default. This is a host-side command - over
        // SysEx or the console - because there is no button to hold at
        // power-on.
        ApplyError restore_defaults(uint32_t now_us);

        // Presets. save() writes the *active* patch, so what comes back is
        // what was running, including any live parameter edits.
        ApplyError save_slot(uint8_t slot);
        ApplyError recall_slot(uint8_t slot, uint32_t now_us);

        // One live parameter write (#20), mirrored into the active patch
        // image so a save or a dump reports what is running. Marks the store
        // dirty rather than writing: a knob sweep must not cost flash.
        ParamError set_param(uint8_t node_index, uint16_t param_index, uint8_t value, uint32_t now_us);

        // Once per main loop: the debounced autosave.
        void service(uint32_t now_us);

        // The patch that is running. Its parameter bytes track live edits.
        const Patch& active() const { return live; }
        const GlobalSettings& globals() const { return live_globals; }
        // The buffer a caller fills before commit(). Pre-loaded with the
        // active patch by begin_edit(), so an incremental edit only has to
        // change the field it cares about.
        Patch& staging() { return stage; }
        GlobalSettings& staging_globals() { return stage_globals; }
        void begin_edit(){ stage = live; stage_globals = live_globals; }

        bool running_defaults() const { return on_defaults; }
        ApplyError last_error() const { return error; }

    private:
        void push_globals();
        void refresh_active_params();

        MixedModeMaster& mm;
        PatchStore& store;
        StatusLeds& leds;
        Patch live;
        Patch stage;
        GlobalSettings live_globals;
        GlobalSettings stage_globals;
        ApplyError error;
        bool on_defaults;
};

#endif
