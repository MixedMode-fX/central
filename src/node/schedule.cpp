#include "node/schedule.h"

// The three domains in one index space, so the ordering is written once
// rather than three times: gate buses first, then note, then CV.
static const uint8_t NOTE_BASE = N_GATE_BUS;
static const uint8_t CV_BASE = N_GATE_BUS + N_NOTE_BUS;

static uint32_t bus_bit(Domain domain, uint8_t bus){
    switch (domain){
        case Domain::Gate: return bus < N_GATE_BUS ? (uint32_t)1u << bus : 0u;
        case Domain::Note: return bus < N_NOTE_BUS ? (uint32_t)1u << (NOTE_BASE + bus) : 0u;
        case Domain::CV:   return bus < N_CV_BUS   ? (uint32_t)1u << (CV_BASE + bus) : 0u;
    }
    return 0u;
}

// Puts one bus of the combined space back into the per-domain set the
// BusManager publishes from.
static void add_bus(BusSet& set, uint8_t bus){
    if (bus < NOTE_BASE)    set.gate |= (uint32_t)1u << bus;
    else if (bus < CV_BASE) set.note = (uint16_t)(set.note | (1u << (bus - NOTE_BASE)));
    else                    set.cv   = (uint16_t)(set.cv   | (1u << (bus - CV_BASE)));
}

Schedule::Schedule() : ports(), order(), publish(), early(), n(0) {
    clear();
}

void Schedule::clear(){
    n = 0;
    for (uint8_t i = 0; i < N_NODE; i++){
        ports[i] = Ports{0, 0, false, false};
        order[i] = i;
        publish[i] = BusSet{0, 0, 0};
    }
    build();
}

void Schedule::set(uint8_t index, const NodeConfig& config, const AlgorithmDescriptor& descriptor){
    if (index >= N_NODE) return;
    Ports& p = ports[index];
    p = Ports{0, 0, descriptor.reads_key, descriptor.writes_key};
    for (uint8_t i = 0; i < descriptor.n_in; i++){
        if (config.in_bus[i] == NO_BUS) continue;
        p.read |= bus_bit(descriptor.in_domain[i], config.in_bus[i]);
    }
    for (uint8_t i = 0; i < descriptor.n_out; i++){
        if (config.out_bus[i] == NO_BUS) continue;
        p.write |= bus_bit(descriptor.out_domain[i], config.out_bus[i]);
    }
    if (index >= n) n = (uint8_t)(index + 1u);
}

uint8_t Schedule::node_at(uint8_t position) const {
    return position < n ? order[position] : 0;
}

const BusSet& Schedule::after(uint8_t position) const {
    static const BusSet none = {0, 0, 0};
    return position < n ? publish[position] : none;
}

void Schedule::build(){
    // Who writes each bus, as a set of nodes - so "the nodes that must run
    // before this one" is an OR over the buses it reads.
    uint64_t writers[N_BUS_TOTAL] = {0};
    for (uint8_t i = 0; i < n; i++){
        for (uint8_t b = 0; b < N_BUS_TOTAL; b++){
            if (ports[i].write & ((uint32_t)1u << b)) writers[b] |= (uint64_t)1u << i;
        }
    }
    // The key, the same way: whoever writes it runs before whoever plays in
    // it. There is one key, so this is one word rather than one per index.
    uint64_t key_writers = 0;
    for (uint8_t i = 0; i < n; i++){
        if (ports[i].writes_key) key_writers |= (uint64_t)1u << i;
    }
    uint64_t needs[N_NODE] = {0};
    for (uint8_t i = 0; i < n; i++){
        uint64_t before_this = 0;
        for (uint8_t b = 0; b < N_BUS_TOTAL; b++){
            if (ports[i].read & ((uint32_t)1u << b)) before_this |= writers[b];
        }
        if (ports[i].reads_key) before_this |= key_writers;
        // A node reading a bus it writes itself is a loop of one: it reads
        // what it published last pass, which is how a NOT oscillates rather
        // than hanging.
        needs[i] = before_this & ~((uint64_t)1u << i);
    }

    // Producers first. When nothing is ready the graph has a loop, and the
    // lowest-index node left goes next: the edges into it that are still
    // unmet are the ones that keep their pass of delay. Lowest index breaks
    // every tie, so a patch always runs in the same order.
    uint64_t placed = 0;
    for (uint8_t pos = 0; pos < n; pos++){
        uint8_t pick = 0xFF;
        for (uint8_t i = 0; i < n && pick == 0xFF; i++){
            if (placed & ((uint64_t)1u << i)) continue;
            if ((needs[i] & ~placed) == 0) pick = i;
        }
        for (uint8_t i = 0; i < n && pick == 0xFF; i++){
            if ((placed & ((uint64_t)1u << i)) == 0) pick = i;
        }
        order[pos] = pick;
        placed |= (uint64_t)1u << pick;
    }

    // A bus is published as soon as its last writer has run; one no node
    // writes is published before the pool runs, which is where a jack's level
    // and the MIDI delivered between passes arrive. Every bus is published
    // exactly once a pass either way, so one nothing writes any more empties
    // rather than holding what it last carried.
    for (uint8_t pos = 0; pos < n; pos++) publish[pos] = BusSet{0, 0, 0};
    early = BusSet{0, 0, 0};
    for (uint8_t b = 0; b < N_BUS_TOTAL; b++){
        uint8_t last = 0xFF;
        for (uint8_t pos = 0; pos < n; pos++){
            if (ports[order[pos]].write & ((uint32_t)1u << b)) last = pos;
        }
        add_bus(last == 0xFF ? early : publish[last], b);
    }
}
