#ifndef MMMC_NODE_PATCH_H
#define MMMC_NODE_PATCH_H

#include <stdint.h>
#include "config.h"
#include "node/node.h"

// A complete module configuration: what every hardware port is connected to,
// and the pool nodes. Plain data, so it can be stored and sent as-is.

enum GatePortDirection : uint8_t {
    GATE_PORT_UNUSED = 0,
    GATE_PORT_IN     = 1,   // jack -> gate bus
    GATE_PORT_OUT    = 2,   // gate bus -> jack
};

struct GatePortConfig {
    uint8_t direction;   // GatePortDirection
    uint8_t bus;         // gate bus index
};

struct MidiInConfig {
    uint8_t source_mask; // MidiPort bits; 0 = unused
    uint8_t channel;     // 0 = omni
    uint8_t bus;         // note bus index
};

struct MidiOutConfig {
    uint8_t target_mask; // MidiPort bits; 0 = unused
    uint8_t channel;     // 0 = keep the event's channel
    uint8_t bus;         // note bus index
};

struct Patch {
    GatePortConfig gate_ports[GPIO_N];
    MidiInConfig   midi_in[N_MIDI_IN_NODES];
    MidiOutConfig  midi_out[N_MIDI_OUT_NODES];
    NodeConfig     nodes[N_NODE];
    uint8_t        n_nodes;
};

// A NodeConfig with every inlet/outlet unconnected and params zeroed.
inline NodeConfig node_config(uint8_t algorithm_id){
    NodeConfig c = {};
    c.algorithm_id = algorithm_id;
    for (uint8_t i = 0; i < MAX_IN; i++) c.in_bus[i] = NO_BUS;
    for (uint8_t i = 0; i < MAX_OUT; i++) c.out_bus[i] = NO_BUS;
    return c;
}

// An empty patch: every port unused, no nodes.
inline Patch empty_patch(){
    Patch p = {};
    for (uint8_t i = 0; i < GPIO_N; i++) p.gate_ports[i] = GatePortConfig{GATE_PORT_UNUSED, NO_BUS};
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++) p.midi_in[i] = MidiInConfig{0, 0, NO_BUS};
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) p.midi_out[i] = MidiOutConfig{0, 0, NO_BUS};
    p.n_nodes = 0;
    return p;
}

#endif
