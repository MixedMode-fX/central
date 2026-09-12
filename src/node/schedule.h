#ifndef MMMC_NODE_SCHEDULE_H
#define MMMC_NODE_SCHEDULE_H

#include <stdint.h>
#include "config.h"
#include "node/node.h"

// The order the pool runs in, and the moment each bus is published.
//
// **Why the order matters.** A bus is published once per pass, and until it
// is, a reader sees the previous pass's value. Run the pool in the order the
// nodes happen to sit in the patch and every hop costs a pass, so two paths
// out of one event arrive at different times: a Metronome advancing an
// Arpeggiator reaches it in one hop, while the chord that same pulse is
// choosing - Metronome, Harmony, Chord - takes three. The arpeggiator then
// steps on the chord it was holding *before* the bar it is playing, which is
// a bar of the wrong harmony on every change and is audible as the figure
// running a step behind the progression. Nothing about that patch is
// unreasonable; the module was simply reading its buses in patch order.
//
// So a node runs **after every node that writes a bus it reads**, and each
// bus is published as soon as its last writer has run. A signal then crosses
// the whole graph within the pass that produced it, whatever it crosses on
// the way, and two paths out of one event arrive together because both arrive
// immediately. Fan-in is unchanged: every writer of a bus has run before any
// reader sees it, so a reader still sees the OR of all writers, the whole
// note queue and the full CV sum, exactly as an order-independent pass did.
//
// **A cycle still costs a pass, and that is the point.** An order where every
// producer precedes its consumers does not exist when the graph has a loop,
// so one edge of each loop keeps the old behaviour: the reader runs before
// the writer and sees the previous pass. Feedback is therefore still a
// well-defined one-pass delay rather than recursion - a NOT wired to its own
// input flickers, it does not hang - and a loop now costs one pass in total
// rather than one per node in it.
//
// **The key is ordered here too.** It is not a bus - it is one control-plane
// value for the whole patch (midi/global_key.h) - but a Key node writing it
// from a note bus and a Chord reading it are producer and consumer in exactly
// the sense above, and left unordered a key change would be heard a pass
// late. So a descriptor's `reads_key` and `writes_key` (node/node.h) join the
// buses a node reads and writes, and everything below - the topological
// order, the loop that costs one pass - is unchanged.
//
// **It is computed once per edit, not per pass.** build() is O(n^2) over the
// pool with everything in bitmasks, and runs on a patch load or on the one
// node an incremental edit reconstructs (#11). A pass walks an array.
struct BusSet {
    uint32_t gate;                  // one bit per gate bus
    uint16_t note;
    uint16_t cv;
};

class Schedule {
    public:
        Schedule();

        // Forgets every node; every bus then publishes before the pool runs,
        // which is what an empty patch needs.
        void clear();
        // Records what one node reads and writes. The descriptor names the
        // domain of each inlet and outlet, so a bus index is read in the
        // right space. Call build() once the pool is complete.
        void set(uint8_t index, const NodeConfig& config, const AlgorithmDescriptor& descriptor);
        // Recomputes the order and the publish points.
        void build();

        uint8_t count() const { return n; }
        // The pool index to run at `position`, which is < count().
        uint8_t node_at(uint8_t position) const;
        // Published before the first node runs: the jacks, the MIDI input
        // delivered between passes, and every bus no node writes.
        const BusSet& before() const { return early; }
        // Published once the node at `position` has run.
        const BusSet& after(uint8_t position) const;

    private:
        // What one node reads and writes: the three domains in one index
        // space (see the .cpp), and the key, which is not a bus and needs no
        // index because there is only one of it.
        struct Ports {
            uint32_t read;
            uint32_t write;
            bool reads_key;
            bool writes_key;
        };

        Ports ports[N_NODE];
        uint8_t order[N_NODE];
        BusSet publish[N_NODE];
        BusSet early;
        uint8_t n;
};

// Every bus of every domain, which is what the ordering counts.
#define N_BUS_TOTAL (N_GATE_BUS + N_NOTE_BUS + N_CV_BUS)

static_assert(N_NODE <= 64, "the scheduler holds a set of nodes in a 64-bit word");
static_assert(N_BUS_TOTAL <= 32, "the scheduler holds a set of buses in a 32-bit word");
static_assert(N_GATE_BUS <= 32, "a gate bus set is a 32-bit word");
static_assert(N_NOTE_BUS <= 16, "a note bus set is a 16-bit word");
static_assert(N_CV_BUS <= 16, "a CV bus set is a 16-bit word");

#endif
