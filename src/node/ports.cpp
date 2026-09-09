#include "node/ports.h"

// GateInPort ----------------------------------------------------------------

void GateInPort::setup(){
    if (enabled()) gpio->mode(port, GPIO_MODE_INPUT_PULLUP);
}

void GateInPort::release(){
    if (enabled()) gpio->mode(port, GPIO_MODE_INPUT_PULLUP);
    bus = NO_BUS;
}

void GateInPort::process(BusManager& buses, uint32_t){
    if (!enabled()) return;
    buses.gate_write(bus, gpio->read(port) != GPIO_LOW);
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
    bus = NO_BUS;
}

void GateOutPort::process(BusManager& buses, uint32_t){
    if (!enabled()) return;
    gpio->write(port, buses.gate_read(bus) ? GPIO_HIGH : GPIO_LOW);
}

// MidiInPort ----------------------------------------------------------------

bool MidiInPort::deliver(BusManager& buses, uint8_t source, const MidiEvent& event) const {
    if (!enabled()) return false;
    if ((source_mask & source) == 0) return false;
    if (channel != 0 && event.channel != channel) return false;
    buses.note_write(bus, event);
    return true;
}

// MidiOutPort ---------------------------------------------------------------

void MidiOutPort::process(BusManager& buses, uint32_t){
    if (!enabled()) return;
    const uint8_t n = buses.note_count(bus);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent& e = buses.note_read(bus, i);
        midi->send(target_mask, e.type, e.data1, e.data2, channel ? channel : e.channel);
    }
}
