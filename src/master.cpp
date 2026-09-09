#include "master.h"
#include "hal/midi_types.h"

MixedModeMaster::MixedModeMaster(IGpio& gpio_if, IMidiOut& midi_if) :
    gpio(gpio_if), midi(midi_if), clk(), bus(), pool(),
    gate_in(), gate_out(), midi_in(), midi_out(),
    tick_pending(false), tick_count(0),
    error(LOAD_OK), node_error(CONFIG_OK), node_error_index(0)
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
    return LOAD_OK;
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
    pool.unload_all();
    for (uint8_t i = 0; i < GPIO_N; i++){ gate_in[i].release(); gate_out[i].release(); }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++) midi_in[i].release();
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].release();
    bus.reset();
    tick_pending = false;
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
    // 4. publish
    bus.swap();
    // 5. hardware outputs
    for (uint8_t i = 0; i < GPIO_N; i++) gate_out[i].process(bus, now_us);
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) midi_out[i].process(bus, now_us);
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

void MixedModeMaster::sync_edge(uint32_t now_us){
    if (clk.source() == MasterClock::CLOCK_CV) clk.external_edge(now_us);
}

void MixedModeMaster::tick(uint32_t count){
    tick_count = count;
    tick_pending = true;
}
