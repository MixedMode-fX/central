#ifndef MMMC_NODE_NODE_H
#define MMMC_NODE_NODE_H

#include <stdint.h>
#include <stddef.h>
#include <new>
#include "config.h"
#include "bus/domain.h"
#include "bus/bus_manager.h"

// Every algorithm and every hardware port is a Node. A node reads and writes
// bus indices; it never names a pin or a transport (only the hardware port
// nodes in node/ports.h hold an IGpio or an IMidiOut).
class Node {
    public:
        virtual ~Node() = default;
        // Once, after the patch is loaded (hardware ports claim their pins).
        virtual void setup() {}
        // Every gate-rate pass. `now_us` is a monotonic microsecond
        // timestamp that may wrap.
        virtual void process(BusManager&, uint32_t) {}
        // On a master clock event, only if the descriptor says wants_tick.
        virtual void tick(BusManager&, uint32_t) {}
        // The node is about to be destroyed (patch swap, #11's handover):
        // emit a note-off for everything it still has sounding, and nothing
        // else. The master swaps and flushes the MIDI outputs afterwards, so
        // what is written here reaches the transports. A node that owns no
        // notes has nothing to do.
        virtual void silence(BusManager&) {}
};

// One pool node's configuration. This is also the preset format.
// Bus indices are interpreted in the domain the algorithm's descriptor
// declares for that inlet/outlet. NO_BUS marks an unconnected optional inlet,
// or an outlet the node writes nowhere (a drum lane with no jack).
struct NodeConfig {
    uint8_t algorithm_id;
    uint8_t in_bus[MAX_IN];
    uint8_t out_bus[MAX_OUT];
    uint8_t params[N_PARAM];
};

// Compile-time description of one algorithm. The validator range-checks a
// NodeConfig against it, the node pool sizes its slots from it, the factory
// constructs from it, and #11 exposes it over SysEx.
struct AlgorithmDescriptor {
    uint8_t       id;
    const char*   name;
    uint8_t       n_in;         // inlets the node has
    uint8_t       min_in;       // inlets that must be connected (the rest may be NO_BUS)
    uint8_t       n_out;
    uint16_t      n_params;
    const Domain* in_domain;    // n_in entries
    const Domain* out_domain;   // n_out entries
    uint16_t      state_size;   // sizeof the node class
    bool          wants_tick;   // subscribes to the master clock
    Node*       (*construct)(void* storage, const NodeConfig&);
};

// Placement-new factory used by every descriptor. Slot overflow is a compile
// error here, never a runtime failure on hardware.
template <class T>
Node* construct_node(void* storage, const NodeConfig& config){
    static_assert(sizeof(T) <= NODE_SLOT_SIZE, "node state does not fit a pool slot; raise NODE_SLOT_SIZE");
    static_assert(alignof(T) <= alignof(max_align_t), "node alignment exceeds the pool slot alignment");
    return new (storage) T(config);
}

#endif
