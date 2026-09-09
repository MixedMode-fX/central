#include "algorithm/switch/sustain.h"
#include "node/registry.h"
#include "hal/midi_types.h"

static const Domain SUSTAIN_IN[1] = {Domain::Gate};
static const Domain SUSTAIN_OUT[1] = {Domain::Note};

const AlgorithmDescriptor Sustain::descriptor = {
    ALGO_SUSTAIN, "Sustain", 1, 1, 1, 3, SUSTAIN_IN, SUSTAIN_OUT, sizeof(Sustain), false, construct_node<Sustain> };

Sustain::Sustain(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    channel(config.params[0] ? config.params[0] : 1),
    controller(config.params[1] ? config.params[1] : 64),
    invert(config.params[2] != 0),
    state(UNKNOWN),
    last_raw(UNKNOWN),
    last_change_us(0)
{}

void Sustain::process(BusManager& bus, uint32_t now_us){
    const bool level = bus.gate_read(in);
    const uint8_t raw = (invert ? !level : level) ? 1 : 0;

    // A mechanical pedal bounces for milliseconds: only accept a new level
    // once it has held for the whole debounce interval.
    if (raw != last_raw){
        last_raw = raw;
        last_change_us = now_us;
        return;
    }
    if (raw != state && (uint32_t)(now_us - last_change_us) >= DEBOUNCE_US){
        state = raw;
        send(bus);
    }
}

void Sustain::send(BusManager& bus) const {
    const MidiEvent e = {MIDI_CONTROL_CHANGE, channel, controller, (uint8_t)(state ? 127 : 0)};
    bus.note_write(out, e);
}
