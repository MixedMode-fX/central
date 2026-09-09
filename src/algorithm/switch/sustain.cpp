#include "sustain.h"


void Sustain::_update(){
    uint8_t new_state = gpio.read(input_pin_index);
    if (new_state != state){
        uint8_t value = Sustain::invert ? (new_state << 6) : (!new_state << 6);
        midi.send(midi_outputs, MIDI_CONTROL_CHANGE, 64, value, 1);
        state = new_state;
    }
}
