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

        // The transport stopped (#4). **A node that holds a note until its
        // next advance edge releases it here**, because the edge may never
        // arrive and a stop is the one moment the module knows that: Harmony
        // holds its root until the next chord, Tonnetz its triad until the
        // next transform, and both hang for ever on a clock that stops.
        //
        // It is opt-in, and the two halves of that are deliberate. A node
        // whose notes follow an input it is still being given does nothing -
        // the input's own note-offs release them, and cutting a key somebody
        // is holding is not what a transport stop means. Nor does a node
        // that plays without an advance edge at all: a self-playing Chord is
        // a drone, and a drone silenced here would never come back, because
        // there is no start hook to bring it back and it would be wrong to
        // add one for something the transport was never driving.
        //
        // Called inside the pass, after process() and before the swap, so the
        // note-offs are published and flushed like any other write - and a
        // node that played on a stale edge this pass is released in the same
        // pass, with the note-off after the note-on it cancels.
        //
        // A stop is not instantaneous in a graph with latency, so the master
        // calls this for as many passes as the graph is deep (see
        // MixedModeMaster::settle_stop) and a node has to tolerate being told
        // more than once. Releasing an empty ledger writes nothing, so an
        // implementation that is simply `silence(bus)` already does.
        virtual void transport_stopped(BusManager&) {}

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

// What kind of thing an algorithm *is*, so a host can group thirty of them
// into a list a musician can read. This is the same split the source tree
// already makes (src/algorithm/logic, .../sequencer, ...) said out loud on
// the wire, because a category worked out in the editor from an algorithm's
// name is a guess that goes stale the moment the firmware gains one.
//
// Never renumber: a host may keep these, and an older one reading a category
// it does not know shows the algorithm under "other" rather than hiding it.
enum AlgorithmCategory : uint8_t {
    CATEGORY_NONE      = 0,   // never shipped: test_params fails on it
    CATEGORY_LOGIC     = 1,   // gates: what a gate bus does to another
    CATEGORY_CLOCK     = 2,   // time: divisions, multiples, note values
    CATEGORY_SEQUENCER = 3,   // patterns: steps, melodies, drum grids
    CATEGORY_MIDI      = 4,   // notes: what happens to them on the way past
    CATEGORY_MODULATOR = 5,   // control signals: shapes and smoothing
    CATEGORY_UTILITY   = 6,   // the rest: plumbing with no better home
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
    // Which shelf of the list this belongs on. One byte, appended to the
    // registry reply after the strings for the same reason they were
    // appended after the name.
    AlgorithmCategory category;
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
