#include "node/node_pool.h"
#include "node/registry.h"

NodePool::NodePool() : slots(), nodes(), descriptors(), n(0) {
    for (uint8_t i = 0; i < N_NODE; i++){ nodes[i] = nullptr; descriptors[i] = nullptr; }
}

NodePool::~NodePool(){
    unload_all();
}

Node* NodePool::load(const NodeConfig& config){
    if (n >= N_NODE) return nullptr;
    const AlgorithmDescriptor* d = registry::find(config.algorithm_id);
    if (d == nullptr) return nullptr;
    Node* node = d->construct(&slots[n][0], config);
    nodes[n] = node;
    descriptors[n] = d;
    n++;
    return node;
}

void NodePool::unload_all(){
    // Reverse order, so a node never outlives one loaded after it.
    while (n > 0){
        n--;
        nodes[n]->~Node();
        nodes[n] = nullptr;
        descriptors[n] = nullptr;
    }
}
