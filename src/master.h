#ifndef MMMC_MASTER_H
#define MMMC_MASTER_H

#include <stdint.h>
#include "config.h"
#include "bus/bus_manager.h"
#include "clock/master_clock.h"
#include "node/node_pool.h"
#include "node/schedule.h"
#include "node/ports.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"

// Why a parameter write was refused (#20). The caller reports; it never
// guesses, and it never silently clamps.
enum ParamError : uint8_t {
    PARAM_SET_OK = 0,
    PARAM_NO_SUCH_NODE,
    PARAM_NO_SUCH_PARAM,      // index beyond n_params, or a reserved byte
    PARAM_VALUE_OUT_OF_RANGE, // outside the ParamDescriptor's [min, max]
    PARAM_REFUSED,            // the node itself refused it
};

enum LoadError : uint8_t {
    LOAD_OK = 0,
    LOAD_TOO_MANY_NODES,
    LOAD_GATE_PORT_BUS_OUT_OF_RANGE,
    LOAD_MIDI_PORT_BUS_OUT_OF_RANGE,
    LOAD_NODE_INVALID,         // see last_node_error() / last_node_index()
    LOAD_DUPLICATE_SINGLETON,  // two nodes of an algorithm there may only be one of
    LOAD_CC_MAPPING_INVALID,   // see last_mapping_index() (#21)
    LOAD_MOD_ROUTE_INVALID,    // see last_route_index()
};

// Owns the master clock, the buses, the reserved hardware port nodes and the
// node pool, and runs the evaluation order every pass:
//   0. collapse whatever subticks the clock produced since the last pass
//   1. hardware input nodes sample and write their buses, which are then
//      published along with every bus no pool node writes
//   2. pool nodes, in the order node/schedule.h computed: each one's
//      process(), then its tick() if a tick fired and it wants one, then the
//      buses whose last writer it is
//   2b. while a stop is settling, the pool releases what the clock was
//       playing (Node::transport_stopped)
//   3. publish whatever those releases wrote, and end the pass
//   4. hardware output nodes read their buses and drive the pins/transports
//
// A node therefore reads what the nodes upstream of it wrote **this** pass,
// so a signal crosses the whole graph in the pass that produced it and two
// paths out of one event - a pulse advancing an arpeggiator, and the chord
// that same pulse chose three nodes away - arrive together. See
// node/schedule.h for why that is the graph's order and what a loop costs.
class MixedModeMaster {
    public:
        MixedModeMaster(IGpio& gpio_if, IMidiOut& midi_if);
        MixedModeMaster(const MixedModeMaster&) = delete;
        MixedModeMaster& operator=(const MixedModeMaster&) = delete;

        // Validates the whole patch first; on any error nothing changes and
        // the current patch keeps running. On success the current patch is
        // unloaded and the new one constructed. Call setup() afterwards.
        LoadError load(const Patch& patch);
        // Handover, then teardown: every pool node releases the notes it
        // owns (Node::silence) and the note-offs are flushed to the
        // transports, then every port returns to a safe state and every
        // pool node is destroyed. A patch swap under a held note therefore
        // cannot hang it downstream (#11, #13).
        void unload();

        // Once after load(): hardware nodes claim their pins.
        void setup();
        // One evaluation pass.
        void pass(uint32_t now_us);

        // The one public entry point for a runtime parameter write (#20).
        // Every control-plane caller - #11's incremental SysEx edits, #21's
        // CC mapping, the console - goes through it, so there is one
        // validator and one set of tests rather than one per transport.
        //
        // The value is range-checked against the ParamDescriptor before it
        // reaches the node. Out of range is **rejected, not clamped**: the
        // node keeps its previous value and the caller is told. A controller
        // never produces an out-of-range value because #21 scales it onto
        // [min, max] first.
        //
        // Called from the main loop between passes, never from an interrupt.
        ParamError set_node_param(uint8_t node_index, uint16_t param_index, uint8_t value);
        // What the node is running. False when the node or the parameter
        // does not exist.
        bool get_node_param(uint8_t node_index, uint16_t param_index, uint8_t& value_out) const;

        // Incremental edits (#11) -------------------------------------------
        //
        // **The state rule**, written down once and relied on everywhere:
        //
        //   * A parameter edit (set_node_param) preserves all node state. A
        //     running sequencer keeps its step position, a divider its phase.
        //   * A connection edit reconstructs **the node whose connection
        //     changed, and only that one**: it gets its handover, releases
        //     what it owns and starts fresh, while every other node in the
        //     patch keeps its state. Nodes copy their bus indices at
        //     construction, so a rebind is a reconstruction; making it
        //     anything else would mean a re-bind seam on all 25 algorithms.
        //   * A port edit reconstructs nothing at all - port nodes are
        //     configured, not constructed.
        //   * A full load() reconstructs everything.
        //
        // Each of these validates before it changes anything, so a rejected
        // edit leaves the running graph exactly as it was.
        LoadError replace_node(uint8_t index, const NodeConfig& config);
        LoadError set_gate_port(uint8_t jack, const GatePortConfig& config);
        LoadError set_midi_in(uint8_t index, const MidiInConfig& config);
        LoadError set_midi_out(uint8_t index, const MidiOutConfig& config);

        // Transport input path (#5): offers an incoming message to every
        // MidiInPort. Returns how many accepted it. System messages never
        // reach a bus: the realtime ones (clock, start, stop, continue) are
        // transport-level and go to the master clock, the rest are dropped,
        // and either way the call returns 0. `now_us` is only read for those.
        uint8_t deliver_midi(uint8_t source, const MidiEvent& event, uint32_t now_us = 0);
        // Whether every MidiInPort that would accept the event has room on
        // its bus for it this pass. The input drain asks before delivering,
        // so a burst larger than a bus - a sustain pedal releasing twenty
        // notes - waits in the queue instead of losing its tail to overflow
        // (#5). A system message never needs room: it reaches the clock, not
        // a bus.
        bool has_room(uint8_t source, const MidiEvent& event) const;
        // A rising edge on the external sync jack (#4).
        void sync_edge(uint32_t now_us);
        // Master clock (#4): the next pass delivers tick() to subscribed
        // nodes. pass() calls this itself from the clock; it stays public so
        // a test can drive the tick directly.
        void tick(uint32_t tick_count);

        MasterClock& clock() { return clk; }
        const MasterClock& clock() const { return clk; }

        // Diagnostics
        LoadError last_error() const { return error; }
        ConfigError last_node_error() const { return node_error; }
        uint8_t last_node_index() const { return node_error_index; }
        uint8_t last_mapping_index() const { return mapping_error_index; }
        uint8_t last_route_index() const { return route_error_index; }
        // Range-checks one controller binding against the patch it belongs
        // to: unknown target, a node index beyond n_nodes, a parameter index
        // beyond the descriptor, min > max. A patch with a bad mapping is
        // rejected whole (#11's rule), never partially applied.
        static bool mapping_valid(const Patch& patch, const CcMapping& mapping);
        // Range-checks one modulation route against the patch it belongs to:
        // a CV bus that exists, a target that exists, min <= max, and **no
        // other route already on that target**. Two routes writing one
        // parameter would be two writers racing over one value with no
        // defined result; two modulators reaching one parameter is two
        // writers on one CV bus, which the bus already sums (bus/domain.h).
        // The rule is enforced here so a patch that would have been
        // unpredictable is refused instead of running.
        // `candidate` is the route being proposed for `slot`; every other
        // slot is read from `patch`. Taking the route separately is what lets
        // an incremental edit be checked against the running patch **without
        // copying it** - a Patch is 12 KB and has no business on the stack.
        static bool route_valid(const Patch& patch, uint8_t slot, const ModRoute& candidate);
        uint8_t node_count() const { return pool.count(); }
        Node* node(uint8_t index) const { return pool.node(index); }
        const AlgorithmDescriptor* node_descriptor(uint8_t index) const { return pool.descriptor(index); }
        const BusManager& buses() const { return bus; }

    private:
        LoadError validate(const Patch& patch);
        // Holds a transport stop against the pool until the gates that were
        // in flight when it stopped have drained.
        void settle_stop();

        IGpio& gpio;
        IMidiOut& midi;
        MasterClock clk;
        BusManager bus;
        NodePool pool;
        Schedule sched;
        GateInPort gate_in[GPIO_N];
        GateOutPort gate_out[GPIO_N];
        MidiInPort midi_in[N_MIDI_IN_NODES];
        MidiOutPort midi_out[N_MIDI_OUT_NODES];
        bool tick_pending;
        // The transport as of the last pass, and the passes left of the
        // settle a stop starts. See settle_stop().
        bool was_running;
        uint8_t stop_settle;
        uint32_t tick_count;
        LoadError error;
        ConfigError node_error;
        uint8_t node_error_index;
        uint8_t mapping_error_index;
        uint8_t route_error_index;
};

#endif
