#ifndef MMMC_NODE_NODE_POOL_H
#define MMMC_NODE_NODE_POOL_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "node/node.h"

// A static array of uniform slots. Any slot holds any algorithm, so there is
// no per-type quota: eight inverters is eight slots with ALGO_LOGIC_NOT.
// No dynamic allocation: nodes are placement-new'd into slots on patch load
// and destroyed explicitly on unload.
class NodePool {
    public:
        NodePool();
        ~NodePool();
        NodePool(const NodePool&) = delete;
        NodePool& operator=(const NodePool&) = delete;

        // Constructs `config` in the next free slot. Assumes the config has
        // been validated; returns nullptr only when the pool is full.
        Node* load(const NodeConfig& config);
        // Destroys every node.
        void unload_all();

        uint8_t count() const { return n; }
        Node* node(uint8_t index) const { return index < n ? nodes[index] : nullptr; }
        const AlgorithmDescriptor* descriptor(uint8_t index) const { return index < n ? descriptors[index] : nullptr; }

    private:
        alignas(max_align_t) uint8_t slots[N_NODE][NODE_SLOT_SIZE];
        Node* nodes[N_NODE];
        const AlgorithmDescriptor* descriptors[N_NODE];
        uint8_t n;
};

#endif
