#include "master.h"
#include "hal/midi_types.h"

MixedModeMaster::MixedModeMaster(IGpio& gpio_if, IMidiOut& midi_if) :
    gpio(gpio_if), midi(midi_if), clk(), bus(), pool(),
    gate_in(), gate_out(), midi_in(), midi_out(),
    tick_pending(false), was_running(clk.running()), stop_settle(0), tick_count(0),
    error(LOAD_OK), node_error(CONFIG_OK), node_error_index(0), mapping_error_index(0),
    route_error_index(0)
{}

LoadError MixedModeMaster::validate(const Patch& patch){
    if (patch.n_nodes > N_NODE) return LOAD_TOO_MANY_NODES;
    for (uint8_t i = 0; i < GPIO_N; i++){
        const GatePortConfig& c = patch.gate_ports[i];
        if (c.direction == GATE_PORT_UNUSED) continue;
        if (c.bus >= N_GATE_BUS) return LOAD_GATE_PORT_BUS_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        const MidiInConfig& c = patch.midi_in[i];
        if (c.source_mask == 0) continue;
        if (c.bus >= N_NOTE_BUS) return LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        const MidiOutConfig& c = patch.midi_out[i];
        if (c.target_mask == 0) continue;
        if (c.bus >= N_NOTE_BUS) return LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    for (uint8_t i = 0; i < patch.n_nodes; i++){
        const ConfigError e = registry::validate(patch.nodes[i]);
        if (e != CONFIG_OK){
            node_error = e;
            node_error_index = i;
            return LOAD_NODE_INVALID;
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
        if (patch.mod_map[i].bus == NO_BUS) continue;
        if (!route_valid(patch, i, patch.mod_map[i])){
            route_error_index = i;
            return LOAD_MOD_ROUTE_INVALID;
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
        default:                  return false;         // CC_TARGET_PORT is reserved
    }
}

bool MixedModeMaster::route_valid(const Patch& patch, uint8_t slot, const ModRoute& r){
    if (slot >= N_MOD_ROUTE) return false;
    if (r.bus == NO_BUS) return true;                   // an unused slot is fine
    if (r.bus >= N_CV_BUS) return false;
    if (r.min > r.max) return false;
    // A transport target is momentary - it fires, it does not hold a value -
    // so there is nothing for a continuous signal to set. A modulator that
    // pressed "start" once a cycle is not a thing to build by accident.
    if (r.target_kind == CC_TARGET_TRANSPORT) return false;
    if (!target_exists(patch, r.target_kind, r.target_index, r.param)) return false;
    for (uint8_t i = 0; i < N_MOD_ROUTE; i++){
        if (i == slot) continue;
        const ModRoute& other = patch.mod_map[i];
        if (other.bus == NO_BUS) continue;
        if (other.target_kind == r.target_kind
         && other.target_index == r.target_index
         && other.param == r.param) return false;       // two writers, one value
    }
    return true;
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
        if (c.direction == GATE_PORT_IN)  gate_in[i].configure(&gpio, i, c.bus);
        if (c.direction == GATE_PORT_OUT) gate_out[i].configure(&gpio, i, c.bus);
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        const MidiInConfig& c = patch.midi_in[i];
        if (c.source_mask != 0) midi_in[i].configure(c.source_mask, c.channel, c.bus);
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        const MidiOutConfig& c = patch.midi_out[i];
        if (c.target_mask != 0) midi_out[i].configure(&midi, c.target_mask, c.channel, c.bus);
    }
    for (uint8_t i = 0; i < patch.n_nodes; i++) pool.load(patch.nodes[i]);
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
        bus.swap();
        for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].process(bus, 0);
    }
    pool.unload_all();
    for (uint8_t i = 0; i < GPIO_N; i++){ gate_in[i].release(); gate_out[i].release(); }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++) midi_in[i].release();
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].release();
    bus.reset();
    tick_pending = false;
    // Nothing of the old patch is in flight any more, so a stop that was
    // still settling has nothing left to settle against.
    stop_settle = 0;
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
    // 1. hardware inputs (MIDI input arrives through deliver_midi() between passes)
    for (uint8_t i = 0; i < GPIO_N; i++) gate_in[i].process(bus, now_us);
    // 2. gate-rate nodes
    const uint8_t n = pool.count();
    for (uint8_t i = 0; i < n; i++) pool.node(i)->process(bus, now_us);
    // 3. clocked nodes
    if (tick_pending){
        tick_pending = false;
        for (uint8_t i = 0; i < n; i++){
            if (pool.descriptor(i)->wants_tick) pool.node(i)->tick(bus, tick_count);
        }
    }
    // 3b. a transport that stopped. A node holding a note until its next
    //     advance edge may never see one again, and this is the one moment
    //     the module knows it (Node::transport_stopped). It runs *after* the
    //     nodes, so a node that played on a stale edge this pass is released
    //     in the same pass rather than left sounding, and before the swap, so
    //     the note-offs go out with this pass's own writes - after the
    //     note-ons they cancel, which is the order a transport needs them in.
    const bool running = clk.running();
    if (!running) settle_stop();
    was_running = running;
    // 4. publish
    bus.swap();
    // 5. hardware outputs
    for (uint8_t i = 0; i < GPIO_N; i++) gate_out[i].process(bus, now_us);
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].process(bus, now_us);
}

// **A stop is not instantaneous in a graph that has latency.** The buses are
// double-buffered, so a gate a clock source wrote before the stop is not read
// until the next pass, and one that runs through a logic node or a gate
// sequencer on the way arrives later still - and the note that last edge
// plays is exactly the note that would be left hanging. So the stop is held
// against the pool for as many passes as the graph can be deep, one per node
// being the worst a chain can manage, and by the end of it whatever was in
// flight has drained. Nothing is silenced after that: a patch advanced from a
// jack has nothing to do with the transport and must keep playing.
void MixedModeMaster::settle_stop(){
    if (was_running) stop_settle = (uint8_t)(pool.count() + 1u);
    if (stop_settle == 0) return;
    stop_settle--;
    for (uint8_t i = 0; i < pool.count(); i++) pool.node(i)->transport_stopped(bus);
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
    bus.swap();
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].process(bus, 0);

    Node* fresh = pool.replace(index, config);
    if (fresh == nullptr) return error = LOAD_NODE_INVALID;
    fresh->setup();
    return error = LOAD_OK;
}

LoadError MixedModeMaster::set_gate_port(uint8_t jack, const GatePortConfig& config){
    if (jack >= GPIO_N) return error = LOAD_GATE_PORT_BUS_OUT_OF_RANGE;
    if (config.direction != GATE_PORT_UNUSED && config.bus >= N_GATE_BUS){
        return error = LOAD_GATE_PORT_BUS_OUT_OF_RANGE;
    }
    // A jack changing direction must stop driving before it starts reading,
    // or a moment of contention is possible on the pin.
    gate_in[jack].release();
    gate_out[jack].release();
    if (config.direction == GATE_PORT_IN){
        gate_in[jack].configure(&gpio, jack, config.bus);
        gate_in[jack].setup();
    } else if (config.direction == GATE_PORT_OUT){
        gate_out[jack].configure(&gpio, jack, config.bus);
        gate_out[jack].setup();
    }
    return error = LOAD_OK;
}

LoadError MixedModeMaster::set_midi_in(uint8_t index, const MidiInConfig& config){
    if (index >= N_MIDI_IN_NODES) return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    if (config.source_mask != 0 && config.bus >= N_NOTE_BUS){
        return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    if (config.source_mask == 0) midi_in[index].release();
    else midi_in[index].configure(config.source_mask, config.channel, config.bus);
    return error = LOAD_OK;
}

LoadError MixedModeMaster::set_midi_out(uint8_t index, const MidiOutConfig& config){
    if (index >= N_MIDI_OUT_NODES) return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    if (config.target_mask != 0 && config.bus >= N_NOTE_BUS){
        return error = LOAD_MIDI_PORT_BUS_OUT_OF_RANGE;
    }
    if (config.target_mask == 0) midi_out[index].release();
    else midi_out[index].configure(&midi, config.target_mask, config.channel, config.bus);
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
        clk.midi_message(event.type, now_us);
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
        if (bus.note_room(midi_in[i].note_bus()) == 0) return false;
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
