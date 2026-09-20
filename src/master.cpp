#include "master.h"
#include "hal/midi_types.h"

MixedModeMaster::MixedModeMaster(IGpio& gpio_if, IMidiOut& midi_if) :
    gpio(gpio_if), midi(midi_if), clk(), clk_out(), bus(), pool(), sched(),
    gate_in(), gate_out(), midi_in(), midi_out(),
    tick_pending(false), was_running(clk.running()), stop_settle(0), tick_count(0),
    error(LOAD_OK), node_error(CONFIG_OK), node_error_index(0), mapping_error_index(0),
    route_error_index(0), dest_error_index(0)
{
    clk_out.attach(midi);
}

LoadError MixedModeMaster::validate(const Patch& patch){
    if (patch.n_nodes > N_NODE) return LOAD_TOO_MANY_NODES;
    for (uint8_t i = 0; i < GPIO_N; i++){
        const GatePortConfig& c = patch.gate_ports[i];
        if (c.direction == GATE_PORT_UNUSED) continue;
        if (!buses_in_range(Domain::Gate, c.buses)) return LOAD_GATE_PORT_BUS_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        const MidiInConfig& c = patch.midi_in[i];
        if (c.source_mask == 0) continue;
        if (!buses_in_range(Domain::Note, c.buses)) return LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        const MidiOutConfig& c = patch.midi_out[i];
        if (c.target_mask == 0) continue;
        if (!buses_in_range(Domain::Note, c.buses)) return LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < patch.n_nodes; i++){
        const ConfigError e = registry::validate(patch.nodes[i]);
        if (e != CONFIG_OK){
            node_error = e;
            node_error_index = i;
            return LOAD_NODE_INVALID;
        }
        // At most one of a singleton algorithm. The Key node is one because
        // the key has one value and two writers would race over it
        // (algorithm/midi/key.h); the check is on the descriptor rather than
        // on the id, so the next one costs nothing here.
        const AlgorithmDescriptor* d = registry::find(patch.nodes[i].algorithm_id);
        if (d == nullptr || !d->singleton) continue;
        for (uint8_t j = 0; j < i; j++){
            if (patch.nodes[j].algorithm_id != patch.nodes[i].algorithm_id) continue;
            node_error_index = i;
            return LOAD_DUPLICATE_SINGLETON;
        }
    }
    for (uint8_t i = 0; i < N_CC_MAP; i++){
        if (patch.cc_map[i].source_mask == 0) continue;
        if (!mapping_valid(patch, patch.cc_map[i])){
            mapping_error_index = i;
            return LOAD_CC_MAPPING_INVALID;
        }
    }
    for (uint8_t i = 0; i < N_MOD_ROUTE; i++){
        if (!patch.mod_map[i].buses.any()) continue;
        if (!route_valid(patch, i, patch.mod_map[i])){
            route_error_index = i;
            return LOAD_MOD_ROUTE_INVALID;
        }
    }
    // The destination pool is shared, so one macro must not be able to eat
    // it: the per-macro cap is what keeps eight macros usable rather than
    // one macro deep and seven empty.
    uint8_t per_macro[N_MACRO] = {};
    for (uint8_t i = 0; i < N_MACRO_DEST; i++){
        const MacroDest& d = patch.macro_dest[i];
        if (d.macro == MACRO_NONE) continue;
        if (!dest_valid(patch, d)){
            dest_error_index = i;
            return LOAD_MACRO_DEST_INVALID;
        }
        if (++per_macro[d.macro] > N_MACRO_DEST_PER_MACRO){
            dest_error_index = i;
            return LOAD_MACRO_DEST_INVALID;
        }
    }
    return LOAD_OK;
}

// Whether a target exists in this patch, shared by a controller binding and a
// modulation route: they reach the same target space, so "no such parameter"
// has to mean the same thing to both.
static bool target_exists(const Patch& patch, uint8_t kind, uint8_t index, uint16_t param){
    switch (kind){
        case CC_TARGET_NODE: {
            if (index >= patch.n_nodes) return false;
            const AlgorithmDescriptor* d = registry::find(patch.nodes[index].algorithm_id);
            if (d == nullptr) return false;
            return registry::param(*d, param) != nullptr;
        }
        case CC_TARGET_CLOCK:     return param < CC_CLOCK_TARGETS;
        case CC_TARGET_TRANSPORT: return param < CC_TRANSPORT_TARGETS;
        // The key exists whatever the patch holds, so unlike a node
        // parameter there is nothing here to check it against.
        case CC_TARGET_KEY:       return param < CC_KEY_TARGETS;
        // A macro exists because the table has that many slots, not because
        // the patch put anything in it; `param` names nothing, since a macro
        // has one value.
        case CC_TARGET_MACRO:     return index < N_MACRO;
        default:                  return false;         // CC_TARGET_PORT is reserved
    }
}

bool MixedModeMaster::route_valid(const Patch& patch, uint8_t slot, const ModRoute& r){
    if (slot >= N_MOD_ROUTE) return false;
    if (!r.buses.any()) return true;                    // an unused slot is fine
    if (!buses_in_range(Domain::CV, r.buses)) return false;
    if (r.min > r.max) return false;
    // A transport target is momentary - it fires, it does not hold a value -
    // so there is nothing for a continuous signal to set. A modulator that
    // pressed "start" once a cycle is not a thing to build by accident.
    if (r.target_kind == CC_TARGET_TRANSPORT) return false;
    // Two routes on one target used to be refused here, because two writers
    // racing over one value has no defined result. They no longer race: an
    // offset route contributes to ControlSum, which sums every contribution
    // on a target and writes once (control/control_sum.h). Macros forced the
    // change - a destination that rises and falls back is two windows on one
    // parameter - and once a parameter can take a sum, refusing a second
    // route is an inconsistency rather than a discipline.
    return target_exists(patch, r.target_kind, r.target_index, r.param);
}

bool MixedModeMaster::dest_valid(const Patch& patch, const MacroDest& d){
    if (d.macro == MACRO_NONE) return true;             // an unused slot is fine
    if (d.macro >= N_MACRO) return false;
    // No recursion: a macro reaching a macro would be a table that writes
    // its own inputs every pass.
    if (d.target_kind == CC_TARGET_MACRO) return false;
    // Momentary, so there is nothing for a swept window to set - the same
    // reason a route refuses one.
    if (d.target_kind == CC_TARGET_TRANSPORT) return false;
    return target_exists(patch, d.target_kind, d.target_index, d.param);
}

bool MixedModeMaster::mapping_valid(const Patch& patch, const CcMapping& m){
    if (m.cc > 119) return false;                       // 120..127 are channel mode
    if (m.channel > 16) return false;
    if (m.min > m.max) return false;
    // A mapping cannot target the control cable: the protocol's own port is
    // not something a patch gets to reach (#11, #21).
    if (m.source_mask & MIDI_CONTROL_PORT) return false;
    return target_exists(patch, m.target_kind, m.target_index, m.param);
}

LoadError MixedModeMaster::load(const Patch& patch){
    node_error = CONFIG_OK;
    node_error_index = 0;
    error = validate(patch);
    if (error != LOAD_OK) return error;

    unload();

    for (uint8_t i = 0; i < GPIO_N; i++){
        const GatePortConfig& c = patch.gate_ports[i];
        if (c.direction == GATE_PORT_IN)  gate_in[i].configure(&gpio, i, c.buses);
        if (c.direction == GATE_PORT_OUT) gate_out[i].configure(&gpio, i, c.buses);
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        const MidiInConfig& c = patch.midi_in[i];
        if (c.source_mask != 0) midi_in[i].configure(c.source_mask, c.channel, c.buses);
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        const MidiOutConfig& c = patch.midi_out[i];
        if (c.target_mask != 0) midi_out[i].configure(&midi, c.target_mask, c.channel, c.buses);
    }
    sched.clear();
    for (uint8_t i = 0; i < patch.n_nodes; i++){
        if (pool.load(patch.nodes[i]) == nullptr) break;   // validated: the pool has room
        sched.set(i, patch.nodes[i], *pool.descriptor(i));
    }
    sched.build();
    return LOAD_OK;
}

void MixedModeMaster::unload(){
    // Handover first: whatever a node still has sounding is released, and
    // the note-offs go out now, before anything is destroyed. The front
    // buffer at this point holds the last pass's events, already delivered
    // in that pass's step 5, so swapping them away loses nothing.
    const uint8_t n = pool.count();
    if (n > 0){
        for (uint8_t i = 0; i < n; i++) pool.node(i)->silence(bus);
        flush_releases();
    }
    pool.unload_all();
    sched.clear();
    for (uint8_t i = 0; i < GPIO_N; i++){ gate_in[i].release(); gate_out[i].release(); }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++) midi_in[i].release();
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].release();
    bus.reset();
    tick_pending = false;
    // A transport edge nobody has been told about yet was the old patch's to
    // hear, like the tick above: the patch that was loaded while a start was
    // in the air did not miss it, it was not there for it.
    clk.take_transport_edges();
    // Nothing of the old patch is in flight any more, so a stop that was
    // still settling has nothing left to settle against.
    stop_settle = 0;
}

void MixedModeMaster::flush_releases(){
    bus.swap();
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].process(bus, 0);
}

void MixedModeMaster::panic(){
    const uint8_t n = pool.count();
    if (n > 0){
        for (uint8_t i = 0; i < n; i++) pool.node(i)->silence(bus);
        flush_releases();
    }
    // Then the sweep, whatever the patch routes and whether or not anything
    // was holding a note: what a panic is for is the note this module has no
    // record of, and a patch that plays nothing at all is exactly the patch
    // loaded over the one that hung it.
    for (uint8_t channel = 1; channel <= MIDI_CHANNELS; channel++){
        midi.send(MIDI_MUSICAL_PORTS, MIDI_CONTROL_CHANGE, MIDI_CC_ALL_NOTES_OFF, 0, channel);
    }
}

void MixedModeMaster::setup(){
    for (uint8_t i = 0; i < GPIO_N; i++){ gate_in[i].setup(); gate_out[i].setup(); }
    for (uint8_t i = 0; i < pool.count(); i++) pool.node(i)->setup();
}

void MixedModeMaster::pass(uint32_t now_us){
    // 0. the clock. Subticks that arrived since the last pass are collapsed
    //    into one tick() with the newest count: a node works from the count,
    //    so nothing is lost, and no node code ever runs in the timer ISR.
    uint32_t count = 0;
    if (clk.consume(count)) tick(count);
    // 1. hardware inputs (MIDI input arrives through deliver_midi() between
    //    passes), and then everything the pool does not write: a jack's level
    //    and that MIDI reach the graph in this pass rather than the next one,
    //    and a bus with no writer left empties.
    for (uint8_t i = 0; i < GPIO_N; i++) gate_in[i].process(bus, now_us);
    bus.publish(sched.before().gate, sched.before().note, sched.before().cv);
    // 2. the pool, in the graph's order: every node runs after the nodes
    //    whose buses it reads, and each bus is published the moment its last
    //    writer has run (node/schedule.h). A node's own tick() follows its
    //    process() as it always has - what changed is that both are published
    //    before anything downstream of it runs.
    const bool ticking = tick_pending;
    tick_pending = false;
    // A transport that stopped. A node holding a note until its next advance
    // edge may never see one again, and this is the one moment the module
    // knows it (Node::transport_stopped). It is told inside the loop below,
    // at its own place in the graph order: *after* its process(), so a node
    // that played on a stale edge this pass is released in the same pass with
    // the note-off after the note-on it cancels, and *before* its outlets are
    // published, so those note-offs cross the graph in the pass that wrote
    // them like any other write. Published after the pool instead and a
    // release reaches the MIDI outputs but no node: the bus's first publish
    // of the next pass clears the front buffer before the reader downstream
    // of it runs, so a Voicer or a NoteDelay never sees the note-off and
    // holds the chord for ever.
    const bool running = clk.running();
    const bool settling = !running && settling_stop();
    was_running = running;
    // And a transport that *moved*, which is not the same question. Taken
    // once here rather than per node, so every node in the pass is told about
    // the same edges, and cleared whether the patch holds a node that cares
    // or not - an edge no node listened to is spent, not queued.
    const uint8_t edges = clk.take_transport_edges();
    // The clock, out of the cables the globals name (clock/clock_out.h).
    // Here rather than with the output port nodes at step 4, because realtime
    // is transport-level and owes nothing to a bus: a host downstream is told
    // the transport moved in the same pass the pool is, and the F8 for this
    // pass's subticks leads the notes the pool is about to play on them.
    clk_out.pass(clk.count(), running, edges);
    const uint8_t n = sched.count();
    for (uint8_t pos = 0; pos < n; pos++){
        const uint8_t i = sched.node_at(pos);
        Node* node = pool.node(i);
        if (node == nullptr) continue;
        node->process(bus, now_us);
        if (ticking && pool.descriptor(i)->wants_tick) node->tick(bus, tick_count);
        if (edges) node->transport_event(bus, edges);
        if (settling) node->transport_stopped(bus);
        bus.publish(sched.after(pos).gate, sched.after(pos).note, sched.after(pos).cv);
    }
    // 3. the end of the pass: anything written to a bus the schedule does not
    //    know about is published too.
    bus.swap();
    // 4. hardware outputs
    for (uint8_t i = 0; i < GPIO_N; i++) gate_out[i].process(bus, now_us);
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].process(bus, now_us);
}

// **A stop is not instantaneous in a graph that has latency.** A bus is
// published once a pass, so a gate a clock source wrote before the stop is
// still read on the pass after it - and the note that last edge plays is
// exactly the note that would be left hanging. Crossing the pool costs
// nothing beyond that pass (node/schedule.h), but a gate going round a loop
// costs one more every time round, so the stop is held against the pool for
// as many passes as the pool has nodes, the longest loop a patch can build,
// and by the end of it whatever was in flight has drained. Nothing is
// silenced after that: a patch advanced from a jack has nothing to do with
// the transport and must keep playing.
bool MixedModeMaster::settling_stop(){
    if (was_running) stop_settle = (uint8_t)(pool.count() + 1u);
    if (stop_settle == 0) return false;
    stop_settle--;
    return true;
}

LoadError MixedModeMaster::replace_node(uint8_t index, const NodeConfig& config){
    if (index >= pool.count()) return error = LOAD_NODE_INVALID;
    const ConfigError e = registry::validate(config);
    if (e != CONFIG_OK){
        node_error = e;
        node_error_index = index;
        return error = LOAD_NODE_INVALID;
    }
    // Handover at node scope: the node about to go releases what it has
    // sounding and the note-offs reach the transports before it is destroyed,
    // exactly as unload() does at patch scope.
    Node* old = pool.node(index);
    old->silence(bus);
    flush_releases();

    Node* fresh = pool.replace(index, config);
    if (fresh == nullptr) return error = LOAD_NODE_INVALID;
    fresh->setup();
    // A connection edit can move the whole graph's order, not just this
    // node's place in it.
    sched.set(index, config, *pool.descriptor(index));
    sched.build();
    return error = LOAD_OK;
}

LoadError MixedModeMaster::set_gate_port(uint8_t jack, const GatePortConfig& config){
    if (jack >= GPIO_N) return error = LOAD_GATE_PORT_BUS_OUT_OF_RANGE;
    if (config.direction != GATE_PORT_UNUSED && !buses_in_range(Domain::Gate, config.buses)){
        return error = LOAD_GATE_PORT_BUS_OUT_OF_RANGE;
    }
    // A jack changing direction must stop driving before it starts reading,
    // or a moment of contention is possible on the pin.
    gate_in[jack].release();
    gate_out[jack].release();
    if (config.direction == GATE_PORT_IN){
        gate_in[jack].configure(&gpio, jack, config.buses);
        gate_in[jack].setup();
    } else if (config.direction == GATE_PORT_OUT){
        gate_out[jack].configure(&gpio, jack, config.buses);
        gate_out[jack].setup();
    }
    return error = LOAD_OK;
}

LoadError MixedModeMaster::set_midi_in(uint8_t index, const MidiInConfig& config){
    if (index >= N_MIDI_IN_NODES) return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    if (config.source_mask != 0 && !buses_in_range(Domain::Note, config.buses)){
        return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    if (config.source_mask == 0) midi_in[index].release();
    else midi_in[index].configure(config.source_mask, config.channel, config.buses);
    return error = LOAD_OK;
}

LoadError MixedModeMaster::set_midi_out(uint8_t index, const MidiOutConfig& config){
    if (index >= N_MIDI_OUT_NODES) return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    if (config.target_mask != 0 && !buses_in_range(Domain::Note, config.buses)){
        return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    if (config.target_mask == 0) midi_out[index].release();
    else midi_out[index].configure(&midi, config.target_mask, config.channel, config.buses);
    return error = LOAD_OK;
}

ParamError MixedModeMaster::set_node_param(uint8_t node_index, uint16_t param_index, uint8_t value){
    Node* n = pool.node(node_index);
    const AlgorithmDescriptor* d = pool.descriptor(node_index);
    if (n == nullptr || d == nullptr) return PARAM_NO_SUCH_NODE;

    const ParamDescriptor* p = registry::param(*d, param_index);
    if (p == nullptr) return PARAM_NO_SUCH_PARAM;
    if (!registry::param_in_range(*p, value)) return PARAM_VALUE_OUT_OF_RANGE;
    return n->set_param(param_index, value) ? PARAM_SET_OK : PARAM_REFUSED;
}

bool MixedModeMaster::get_node_param(uint8_t node_index, uint16_t param_index, uint8_t& value_out) const {
    const Node* n = pool.node(node_index);
    const AlgorithmDescriptor* d = pool.descriptor(node_index);
    if (n == nullptr || d == nullptr) return false;
    if (registry::param(*d, param_index) == nullptr) return false;
    value_out = n->get_param(param_index);
    return true;
}

uint8_t MixedModeMaster::deliver_midi(uint8_t source, const MidiEvent& event, uint32_t now_us){
    // System messages are not channel-voice traffic and have no business on
    // a note bus: a MidiOutPort would re-send them with a channel attached.
    // The realtime ones the clock understands go to the clock; the rest -
    // SysEx, song position, active sensing - are dropped here until something
    // asks for them (#11 for SysEx).
    if (event.type >= 0xF0){
        clk.midi_message(source, event.type, now_us);
        return 0;
    }
    uint8_t accepted = 0;
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        if (midi_in[i].deliver(bus, source, event)) accepted++;
    }
    return accepted;
}

bool MixedModeMaster::has_room(uint8_t source, const MidiEvent& event) const {
    if (event.type >= 0xF0) return true;
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        if (!midi_in[i].accepts(source, event)) continue;
        if (bus.note_room(midi_in[i].note_buses()) == 0) return false;
    }
    return true;
}

void MixedModeMaster::sync_edge(uint32_t now_us){
    if (clk.source() == MasterClock::CLOCK_CV) clk.external_edge(now_us);
}

void MixedModeMaster::tick(uint32_t count){
    tick_count = count;
    tick_pending = true;
}
