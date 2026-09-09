#include "patch/patch_manager.h"
#include "patch/default_patch.h"

PatchManager::PatchManager(MixedModeMaster& master, PatchStore& patch_store, StatusLeds& status) :
    mm(master), store(patch_store), leds(status),
    live(empty_patch()), stage(empty_patch()),
    live_globals(default_globals()), stage_globals(default_globals()),
    error(APPLY_OK), on_defaults(false)
{}

void PatchManager::push_globals(){
    mm.clock().set_source(live_globals.clock_source);
    mm.clock().set_bpm(live_globals.bpm);
    mm.clock().set_cv_ppqn(live_globals.cv_ppqn);
}

// After a load the nodes have applied their own zero-means-default rules, so
// the active image is refreshed from what each node is actually running.
// Then a save round-trips to the same graph, and a dump reports the running
// value rather than the byte the patch happened to carry (#20).
void PatchManager::refresh_active_params(){
    for (uint8_t n = 0; n < mm.node_count(); n++){
        const AlgorithmDescriptor* d = mm.node_descriptor(n);
        if (d == nullptr) continue;
        for (uint16_t p = 0; p < d->n_params && p < N_PARAM; p++){
            uint8_t v = 0;
            if (mm.get_node_param(n, p, v)) live.nodes[n].params[p] = v;
        }
    }
}

ApplyError PatchManager::commit(uint32_t now_us){
    // Validated whole before anything is touched: on failure the running
    // patch keeps running and nothing has been half-applied.
    const LoadError e = mm.load(stage);
    if (e != LOAD_OK){
        leds.error(now_us);
        return error = APPLY_INVALID;
    }
    mm.setup();
    live = stage;
    live_globals = stage_globals;
    push_globals();
    refresh_active_params();
    on_defaults = false;
    leds.set_running_defaults(false);
    store.mark_dirty(now_us);
    return error = APPLY_OK;
}

ApplyError PatchManager::apply(const Patch& patch, const GlobalSettings& g, uint32_t now_us){
    stage = patch;
    stage_globals = g;
    return commit(now_us);
}

ApplyError PatchManager::restore_defaults(uint32_t now_us){
    const ApplyError e = apply(default_patch(), default_globals_for_patch(), now_us);
    // Running the default because it was asked for is not the same state as
    // running it because nothing valid was stored: the red LED stays off.
    return e;
}

void PatchManager::boot(uint32_t now_us){
    Patch stored;
    GlobalSettings stored_globals;
    const StoreError s = store.load(0, stored, stored_globals);
    if (s == STORE_OK && apply(stored, stored_globals, now_us) == APPLY_OK){
        leds.identify(now_us);                 // a power cycle is visible
        return;
    }

    // Nothing valid stored, or it failed to validate: run the default and say
    // so on the red LED. Never run a partially-valid patch.
    apply(default_patch(), default_globals_for_patch(), now_us);
    on_defaults = true;
    leds.set_running_defaults(s != STORE_EMPTY);
    // An empty store is a new module, not a fault, so it boots green; a
    // corrupt one lights red until a valid patch arrives.
    if (s != STORE_EMPTY) leds.error(now_us);
    store.mark_dirty(now_us);
    leds.identify(now_us);
}

ApplyError PatchManager::save_slot(uint8_t slot){
    if (slot >= PATCH_SLOTS) return error = APPLY_NO_SUCH_SLOT;
    const StoreError s = store.save(slot, live, live_globals);
    if (s == STORE_TOO_LARGE) return error = APPLY_TOO_LARGE;
    return error = (s == STORE_OK ? APPLY_OK : APPLY_INVALID);
}

ApplyError PatchManager::recall_slot(uint8_t slot, uint32_t now_us){
    if (slot >= PATCH_SLOTS) return error = APPLY_NO_SUCH_SLOT;
    Patch p;
    GlobalSettings g;
    const StoreError s = store.load(slot, p, g);
    if (s == STORE_EMPTY) return error = APPLY_SLOT_EMPTY;
    if (s != STORE_OK) return error = APPLY_SLOT_CORRUPT;
    return apply(p, g, now_us);
}

ParamError PatchManager::set_param(uint8_t node_index, uint16_t param_index,
                                   uint8_t value, uint32_t now_us){
    const ParamError e = mm.set_node_param(node_index, param_index, value);
    if (e != PARAM_SET_OK){
        leds.error(now_us);
        return e;
    }
    // Mirror what the node actually took, which may not be the byte that was
    // sent: a node applies its own zero-means-default rule.
    uint8_t running = value;
    mm.get_node_param(node_index, param_index, running);
    if (node_index < N_NODE && param_index < N_PARAM) live.nodes[node_index].params[param_index] = running;
    store.mark_dirty(now_us);
    return PARAM_SET_OK;
}

void PatchManager::service(uint32_t now_us){
    store.service(now_us, live, live_globals);
}
