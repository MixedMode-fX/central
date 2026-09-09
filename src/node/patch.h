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

// What a controller binding reaches (#21).
//
// The master clock is not a node - tempo, source and transport live on
// MasterClock, outside Patch::nodes[] - so a target space of "node index plus
// parameter index" could not reach the one thing a knob is most obviously
// for. Hence a kind.
enum CcTargetKind : uint8_t {
    CC_TARGET_NODE      = 0,   // target_index is a node, param a parameter
    CC_TARGET_CLOCK     = 1,   // param selects tempo / source / cv ppqn
    CC_TARGET_TRANSPORT = 2,   // param selects start / stop / continue / tap
    CC_TARGET_PORT      = 3,   // reserved: a MidiInPort / MidiOutPort field
    CC_TARGET_KINDS,
};

// What a CC_TARGET_CLOCK mapping's `param` selects.
enum CcClockTarget : uint8_t {
    CC_CLOCK_TEMPO  = 0,
    CC_CLOCK_SOURCE = 1,
    CC_CLOCK_PPQN   = 2,
    CC_CLOCK_TARGETS,
};

// What a CC_TARGET_TRANSPORT mapping's `param` selects. These are momentary:
// they fire once when the CC crosses 64 going up, and do nothing on the way
// back down, which is how a momentary button behaves.
enum CcTransportTarget : uint8_t {
    CC_TRANSPORT_START    = 0,
    CC_TRANSPORT_STOP     = 1,
    CC_TRANSPORT_CONTINUE = 2,
    CC_TRANSPORT_TAP      = 3,
    CC_TRANSPORT_TARGETS,
};

// Mapping flags, packed into one byte.
enum CcFlags : uint8_t {
    // How a knob that is out of step with its target catches up. See
    // control/cc_mapper.h for why each exists.
    CC_TAKEOVER_MASK  = 0x03,
    CC_TAKEOVER_JUMP  = 0x00,   // the default: always responds
    CC_TAKEOVER_PICKUP = 0x01,
    CC_TAKEOVER_SCALE = 0x02,

    // Endless encoders send an increment, not a position, in one of three
    // incompatible encodings. A relative mapping sidesteps takeover entirely.
    CC_RELATIVE_MASK      = 0x0C,
    CC_ABSOLUTE           = 0x00,
    CC_RELATIVE_TWOS      = 0x04,   // 1..63 up, 127..65 down
    CC_RELATIVE_SIGNED    = 0x08,   // 1..63 up, 65..127 down
    CC_RELATIVE_OFFSET64  = 0x0C,   // 65..127 up, 63..1 down

    // This CC is the MSB of a 14-bit pair; CC + 32 is the LSB.
    CC_FOURTEEN_BIT = 0x10,
    // Forward the CC to the graph as well as acting on it. Off by default:
    // the user bound this CC deliberately.
    CC_PASS_THROUGH = 0x20,
};

// One controller binding. Part of the patch, not part of the graph: a
// parameter is not a bus signal, it has no domain and no per-pass value, and
// writing one is a control-plane operation (#21).
struct CcMapping {
    uint8_t  source_mask;   // MidiPort bits, as MidiInConfig; 0 = unused
    uint8_t  channel;       // 1..16, 0 = omni
    uint8_t  cc;            // 0..119; 120..127 are channel mode messages
    uint8_t  target_kind;   // CcTargetKind
    uint8_t  target_index;  // node index, or unused
    uint16_t param;         // parameter index, or a clock / transport target
    // The sub-range of the target the controller sweeps, in the **target's
    // own units** - a parameter's value, or BPM for the clock's tempo. One
    // rule for every target, which is why these are 16-bit: CLOCK_MAX_BPM
    // does not fit a byte. min == max == 0 means the target's full range.
    uint16_t min;
    uint16_t max;
    uint8_t  flags;         // CcFlags
};

inline CcMapping unused_mapping(){
    CcMapping m = {};
    m.source_mask = 0;
    return m;
}

struct Patch {
    GatePortConfig gate_ports[GPIO_N];
    MidiInConfig   midi_in[N_MIDI_IN_NODES];
    MidiOutConfig  midi_out[N_MIDI_OUT_NODES];
    NodeConfig     nodes[N_NODE];
    CcMapping      cc_map[N_CC_MAP];
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
    for (uint8_t i = 0; i < N_CC_MAP; i++) p.cc_map[i] = unused_mapping();
    p.n_nodes = 0;
    return p;
}

#endif
