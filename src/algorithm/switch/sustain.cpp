#include "algorithm/switch/sustain.h"
#include "node/registry.h"
#include "hal/midi_types.h"

static const Domain SUSTAIN_IN[1] = {Domain::Gate};
static const Domain SUSTAIN_OUT[1] = {Domain::Note};

static const ParamDescriptor SUSTAIN_PARAMS[3] = {
    {"channel",    1, 16,  1, PARAM_CHANNEL, nullptr},
    {"controller", 0, 127, 64, PARAM_NUMBER, nullptr},
    {"invert",     0, 1,   0, PARAM_BOOL,    nullptr},
};
static const ParamGroup SUSTAIN_GROUPS[1] = {{0, 1, 3, SUSTAIN_PARAMS}};

const AlgorithmDescriptor Sustain::descriptor = {
    ALGO_SUSTAIN, "Sustain", 1, 1, 1, 3, SUSTAIN_IN, SUSTAIN_OUT, sizeof(Sustain), false, construct_node<Sustain>,
    SUSTAIN_GROUPS, 1 };

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

// Inverting the pedal re-reads as a level change and settles through the
// debounce like any other, so nothing special is needed here.
bool Sustain::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: channel = value ? value : 1; return true;
        case 1: controller = value; return true;
        case 2: invert = (value != 0); return true;
        default: return false;
    }
}

uint8_t Sustain::get_param(uint16_t index) const {
    switch (index){
        case 0: return channel;
        case 1: return controller;
        case 2: return invert ? 1 : 0;
        default: return 0;
    }
}

void Sustain::send(BusManager& bus) const {
    const MidiEvent e = {MIDI_CONTROL_CHANGE, channel, controller, (uint8_t)(state ? 127 : 0)};
    bus.note_write(out, e);
}
