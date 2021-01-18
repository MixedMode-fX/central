#include "sustain.h"

Sustain::Sustain(uint8_t pedal_pin, uint8_t midi_outputs){
    Sustain::pedal_pin = pedal_pin;
    Sustain::midi_outputs = midi_outputs;
    gpioMode(pedal_pin, INPUT_PULLUP);
}

Sustain::~Sustain(){
    gpioMode(pedal_pin, OUTPUT);
    gpioDigitalWrite(pedal_pin, LOW);
}

void Sustain::update(){
    uint8_t new_state = gpioDigitalRead(pedal_pin);
    if (new_state != state){
        mm_send(midi_outputs, midi::ControlChange, 64, !new_state << 6, 1);
        state = new_state;
    }
}