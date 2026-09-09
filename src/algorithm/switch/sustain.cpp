#include "sustain.h"

void Sustain::_setup(){
    const uint8_t raw = gpio.read(input_port);
    state = last_raw = invert ? !raw : raw;
    send_state();
}

void Sustain::_update(uint32_t now_us){
    const uint8_t raw_in = gpio.read(input_port);
    const uint8_t raw = invert ? !raw_in : raw_in;

    // A mechanical pedal bounces for milliseconds: only accept a new level
    // once it has held for the whole debounce interval.
    if (raw != last_raw){
        last_raw = raw;
        last_change_us = now_us;
        return;
    }
    if (raw != state && (uint32_t)(now_us - last_change_us) >= debounce_us){
        state = raw;
        send_state();
    }
}

void Sustain::send_state(){
    midi.send(targets, MIDI_CONTROL_CHANGE, controller, state ? 127 : 0, channel);
}
