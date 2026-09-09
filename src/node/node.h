#ifndef MMMC_NODE_NODE_H
#define MMMC_NODE_NODE_H

#include <stdint.h>
#include <stddef.h>
#include <new>
#include "config.h"
#include "bus/domain.h"
#include "bus/bus_manager.h"
#include "node/param.h"

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

        // Set one parameter after construction (#20). `index` is < the
        // descriptor's n_params; `value` has already been range-checked
        // against the ParamDescriptor by MixedModeMaster::set_node_param,
        // which is the one entry point every control-plane caller uses.
        // Returns false if the node itself refused the write - the caller
        // reports, it never guesses.
        //
        // A subclass overrides this to re-derive whatever it cached in its
        // constructor: EuclidianSequencer recomputes its pattern, ClockDiv
        // its period. Re-derivation is explicit, per algorithm, and a set
        // that does not change the value returns early - a knob sweep is
        // ~100 messages a second per controller (#21) and nothing should pay
        // for Bjorklund on every one of them.
        //
        // Never called from interrupt context: the transport enqueues, the
        // pass applies, before process().
        virtual bool set_param(uint16_t index, uint8_t value){ (void)index; (void)value; return false; }
        // What the node is *running*, not what the patch said at load time,
        // so #11's dump is honest for free.
        virtual uint8_t get_param(uint16_t index) const { (void)index; return 0; }
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
//
// **An inlet's name is not decoration.** A patch is a list of what each node
// reads and writes, so a host that can only say "in 0" and "in 1" is asking a
// user to guess which one advances the sequencer and which one resets it.
// The domain narrows the choice to the legal buses; the name is what says
// what the connection *means*. Every parameter has carried a name since #20
// for exactly this reason - these close the same gap for the ports and for
// the algorithm itself, and they travel over the same registry messages, so
// an algorithm added to the firmware still arrives in the editor described.
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
    const ParamGroup* param_groups;   // n_param_groups entries; covers n_params
    uint8_t       n_param_groups;
    // n_in / n_out entries, and one line saying what the algorithm is for.
    // Nullptr is legal and means "no better name than the index" - but an
    // algorithm that ships without these is one a user has to read the source
    // to patch, so test_params asserts every entry in the table has them.
    //
    // Cost, measured rather than guessed, the same way param.h reports the
    // parameter descriptors': describing all 25 algorithms' ports adds
    // **about 3 KB of flash** (2048 bytes of string data, the rest the code
    // that puts them on the wire) against the Teensy 4.1's 8 MB. If that ever
    // becomes uncomfortable it is these strings that go behind a build flag,
    // never the domains, because the domains are what the validator enforces.
    const char* const* in_name;
    const char* const* out_name;
    const char*   summary;
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
