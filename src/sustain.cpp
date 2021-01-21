#include "sustain.h"


void Sustain::update(){
    uint8_t new_state = gpioDigitalRead(input_pin_index);
    if (new_state != state){
        uint8_t value = Sustain::invert ? (new_state << 6) : (!new_state << 6);
        mm_send(midi_outputs, midi::ControlChange, 64, value, 1);
        state = new_state;
    }
}