#ifndef MMMC_NODE_PORTS_H
#define MMMC_NODE_PORTS_H

#include <stdint.h>
#include "node/node.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"

// The four hardware port node types. They live in reserved slots owned by
// MixedModeMaster, outside the pool, and are the only nodes that hold an
// IGpio or an IMidiOut. Each is configured (not constructed) on patch load.

// Samples one jack every pass and writes its level to every gate bus it is on.
class GateInPort : public Node {
    public:
        GateInPort() : gpio(nullptr), port(0), buses() {}
        GateInPort(const GateInPort&) = delete;
        GateInPort& operator=(const GateInPort&) = delete;

        void configure(IGpio* gpio_if, uint8_t port_index, BusSet gate_buses){
            gpio = gpio_if; port = port_index; buses = gate_buses;
        }
        void release();
        // In use: claimed by a patch, whether or not it is patched to
        // anything. A jack on no bus still holds its pin.
        bool enabled() const { return gpio != nullptr; }

        void setup() override;
        void process(BusManager& bus, uint32_t) override;

    private:
        IGpio* gpio;
        uint8_t port;
        BusSet buses;
};

// Reads its gate buses after the swap - the OR of them - and drives one jack.
class GateOutPort : public Node {
    public:
        GateOutPort() : gpio(nullptr), port(0), buses() {}
        GateOutPort(const GateOutPort&) = delete;
        GateOutPort& operator=(const GateOutPort&) = delete;

        void configure(IGpio* gpio_if, uint8_t port_index, BusSet gate_buses){
            gpio = gpio_if; port = port_index; buses = gate_buses;
        }
        void release();
        bool enabled() const { return gpio != nullptr; }

        void setup() override;
        void process(BusManager& bus, uint32_t) override;

    private:
        IGpio* gpio;
        uint8_t port;
        BusSet buses;
};

// Receives MIDI from the transports whose bit is in `source_mask`, filtered
// by channel (0 = omni), and writes every note bus it is on. Events are
// delivered by MixedModeMaster::deliver_midi(), fed by the transport queue
// (#5).
class MidiInPort : public Node {
    public:
        MidiInPort() : source_mask(0), channel(0), buses() {}

        void configure(uint8_t sources, uint8_t channel_filter, BusSet note_buses){
            source_mask = sources; channel = channel_filter; buses = note_buses;
        }
        void release(){ source_mask = 0; buses = BusSet{}; }
        bool enabled() const { return source_mask != 0; }

        // Whether this port's filter takes the event: enabled, the source is
        // in its mask, and the channel matches (or the port is omni).
        bool accepts(uint8_t source, const MidiEvent& event) const;
        // The note buses an accepted event is written to.
        BusSet note_buses() const { return buses; }
        // Writes the event to those buses if accepts() says so. Returns true
        // if it was accepted, whether or not they had room for it.
        bool deliver(BusManager& bus, uint8_t source, const MidiEvent& event) const;

    private:
        uint8_t source_mask;
        uint8_t channel;
        BusSet buses;
};

// Reads its note buses after the swap and sends every event to the transports
// in `target_mask`, optionally forcing the channel (0 = keep the event's).
class MidiOutPort : public Node {
    public:
        MidiOutPort() : midi(nullptr), target_mask(0), channel(0), buses() {}
        MidiOutPort(const MidiOutPort&) = delete;
        MidiOutPort& operator=(const MidiOutPort&) = delete;

        void configure(IMidiOut* midi_if, uint8_t targets, uint8_t channel_override, BusSet note_buses){
            midi = midi_if; target_mask = targets; channel = channel_override; buses = note_buses;
        }
        void release(){ target_mask = 0; buses = BusSet{}; }
        bool enabled() const { return midi != nullptr && target_mask != 0; }

        void process(BusManager& bus, uint32_t) override;

    private:
        IMidiOut* midi;
        uint8_t target_mask;
        uint8_t channel;
        BusSet buses;
};

#endif
