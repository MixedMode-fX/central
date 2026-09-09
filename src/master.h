#ifndef MMMC_MASTER_H
#define MMMC_MASTER_H

#include <stdint.h>
#include "config.h"
#include "bus/bus_manager.h"
#include "clock/master_clock.h"
#include "node/node_pool.h"
#include "node/ports.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"

enum LoadError : uint8_t {
    LOAD_OK = 0,
    LOAD_TOO_MANY_NODES,
    LOAD_GATE_PORT_BUS_OUT_OF_RANGE,
    LOAD_MIDI_PORT_BUS_OUT_OF_RANGE,
    LOAD_NODE_INVALID,         // see last_node_error() / last_node_index()
};

// Owns the master clock, the buses, the reserved hardware port nodes and the
// node pool, and runs the evaluation order every pass:
//   0. collapse whatever subticks the clock produced since the last pass
//   1. hardware input nodes sample and write their buses
//   2. pool nodes process()
//   3. if a clock tick fired, nodes that want it tick()
//   4. swap buffers
//   5. hardware output nodes read their buses and drive the pins/transports
class MixedModeMaster {
    public:
        MixedModeMaster(IGpio& gpio_if, IMidiOut& midi_if);
        MixedModeMaster(const MixedModeMaster&) = delete;
        MixedModeMaster& operator=(const MixedModeMaster&) = delete;

        // Validates the whole patch first; on any error nothing changes and
        // the current patch keeps running. On success the current patch is
        // unloaded and the new one constructed. Call setup() afterwards.
        LoadError load(const Patch& patch);
        // Returns every port to a safe state and destroys every pool node.
        void unload();

        // Once after load(): hardware nodes claim their pins.
        void setup();
        // One evaluation pass.
        void pass(uint32_t now_us);

        // Transport input path (#5): offers an incoming message to every
        // MidiInPort. Returns how many accepted it. System messages never
        // reach a bus: the realtime ones (clock, start, stop, continue) are
        // transport-level and go to the master clock, the rest are dropped,
        // and either way the call returns 0. `now_us` is only read for those.
        uint8_t deliver_midi(uint8_t source, const MidiEvent& event, uint32_t now_us = 0);
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
        uint8_t node_count() const { return pool.count(); }
        const BusManager& buses() const { return bus; }

    private:
        LoadError validate(const Patch& patch);

        IGpio& gpio;
        IMidiOut& midi;
        MasterClock clk;
        BusManager bus;
        NodePool pool;
        GateInPort gate_in[GPIO_N];
        GateOutPort gate_out[GPIO_N];
        MidiInPort midi_in[N_MIDI_IN_NODES];
        MidiOutPort midi_out[N_MIDI_OUT_NODES];
        bool tick_pending;
        uint32_t tick_count;
        LoadError error;
        ConfigError node_error;
        uint8_t node_error_index;
};

#endif
