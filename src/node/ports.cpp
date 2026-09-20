#include "node/ports.h"

// GateInPort ----------------------------------------------------------------

void GateInPort::setup(){
    if (enabled()) gpio->mode(port, GPIO_MODE_INPUT_PULLUP);
}

void GateInPort::release(){
    if (enabled()) gpio->mode(port, GPIO_MODE_INPUT_PULLUP);
    gpio = nullptr;
    buses = BusSet{};
}

void GateInPort::process(BusManager& bus, uint32_t){
    if (!enabled()) return;
    bus.gate_write(buses, gpio->read(port) != GPIO_LOW);
}

// GateOutPort ---------------------------------------------------------------

void GateOutPort::setup(){
    if (!enabled()) return;
    gpio->mode(port, GPIO_MODE_OUTPUT);
    gpio->write(port, GPIO_LOW);
}

void GateOutPort::release(){
    if (enabled()){
        gpio->write(port, GPIO_LOW);
        gpio->mode(port, GPIO_MODE_INPUT_PULLUP);
    }
    gpio = nullptr;
    buses = BusSet{};
}

void GateOutPort::process(BusManager& bus, uint32_t){
    if (!enabled()) return;
    gpio->write(port, bus.gate_read(buses) ? GPIO_HIGH : GPIO_LOW);
}

// MidiInPort ----------------------------------------------------------------

bool MidiInPort::accepts(uint8_t source, const MidiEvent& event) const {
    if (!enabled()) return false;
    if ((source_mask & source) == 0) return false;
    if (channel != 0 && event.channel != channel) return false;
    return true;
}

bool MidiInPort::deliver(BusManager& bus, uint8_t source, const MidiEvent& event) const {
    if (!accepts(source, event)) return false;
    bus.note_write(buses, event);
    return true;
}

// MidiOutPort ---------------------------------------------------------------

void MidiOutPort::process(BusManager& bus, uint32_t){
    if (!enabled()) return;
    const uint8_t n = bus.note_count(buses);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent& e = bus.note_read(buses, i);
        midi->send(target_mask, e.type, e.data1, e.data2, channel ? channel : e.channel);
    }
}
