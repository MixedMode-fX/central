#ifndef MMMC_NODE_PORTS_H
#define MMMC_NODE_PORTS_H

#include <stdint.h>
#include "node/node.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"

// The four hardware port node types. They live in reserved slots owned by
// MixedModeMaster, outside the pool, and are the only nodes that hold an
// IGpio or an IMidiOut. Each is configured (not constructed) on patch load.

// Samples one jack every pass and writes its level to a gate bus.
class GateInPort : public Node {
    public:
        GateInPort() : gpio(nullptr), port(0), bus(NO_BUS) {}
        GateInPort(const GateInPort&) = delete;
        GateInPort& operator=(const GateInPort&) = delete;

        void configure(IGpio* gpio_if, uint8_t port_index, uint8_t gate_bus){
            gpio = gpio_if; port = port_index; bus = gate_bus;
        }
        void release();
        bool enabled() const { return gpio != nullptr && bus != NO_BUS; }

        void setup() override;
        void process(BusManager& buses, uint32_t) override;

    private:
        IGpio* gpio;
        uint8_t port;
        uint8_t bus;
};

// Reads a gate bus after the swap and drives one jack.
class GateOutPort : public Node {
    public:
        GateOutPort() : gpio(nullptr), port(0), bus(NO_BUS) {}
        GateOutPort(const GateOutPort&) = delete;
        GateOutPort& operator=(const GateOutPort&) = delete;

        void configure(IGpio* gpio_if, uint8_t port_index, uint8_t gate_bus){
            gpio = gpio_if; port = port_index; bus = gate_bus;
        }
        void release();
        bool enabled() const { return gpio != nullptr && bus != NO_BUS; }

        void setup() override;
        void process(BusManager& buses, uint32_t) override;

    private:
        IGpio* gpio;
        uint8_t port;
        uint8_t bus;
};

// Receives MIDI from the transports whose bit is in `source_mask`, filtered
// by channel (0 = omni), and writes a note bus. Events are delivered by
// MixedModeMaster::deliver_midi(), fed by the transport queue (#5).
class MidiInPort : public Node {
    public:
        MidiInPort() : source_mask(0), channel(0), bus(NO_BUS) {}

        void configure(uint8_t sources, uint8_t channel_filter, uint8_t note_bus){
            source_mask = sources; channel = channel_filter; bus = note_bus;
        }
        void release(){ source_mask = 0; bus = NO_BUS; }
        bool enabled() const { return source_mask != 0 && bus != NO_BUS; }

        // Whether this port's filter takes the event: enabled, the source is
        // in its mask, and the channel matches (or the port is omni).
        bool accepts(uint8_t source, const MidiEvent& event) const;
        // The note bus an accepted event is written to.
        uint8_t note_bus() const { return bus; }
        // Writes the event to the bus if accepts() says so. Returns true if
        // it was accepted, whether or not the bus had room for it.
        bool deliver(BusManager& buses, uint8_t source, const MidiEvent& event) const;

    private:
        uint8_t source_mask;
        uint8_t channel;
        uint8_t bus;
};

// Reads a note bus after the swap and sends every event to the transports
// in `target_mask`, optionally forcing the channel (0 = keep the event's).
class MidiOutPort : public Node {
    public:
        MidiOutPort() : midi(nullptr), target_mask(0), channel(0), bus(NO_BUS) {}
        MidiOutPort(const MidiOutPort&) = delete;
        MidiOutPort& operator=(const MidiOutPort&) = delete;

        void configure(IMidiOut* midi_if, uint8_t targets, uint8_t channel_override, uint8_t note_bus){
            midi = midi_if; target_mask = targets; channel = channel_override; bus = note_bus;
        }
        void release(){ target_mask = 0; bus = NO_BUS; }
        bool enabled() const { return midi != nullptr && target_mask != 0 && bus != NO_BUS; }

        void process(BusManager& buses, uint32_t) override;

    private:
        IMidiOut* midi;
        uint8_t target_mask;
        uint8_t channel;
        uint8_t bus;
};

#endif
