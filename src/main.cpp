#include <Arduino.h>

#include "hardware.h"
#include "gpio.h"
#include "mm_midi.h"
#include "sustain.h"

uint8_t GPIO[GPIO_N] = {GPIO_PINS};
uint8_t GPIO_MODES[GPIO_N] = {1,1,1,1,1,1,1,1}; // set all GPIO to outputs as default


void setup(){
    Serial.begin(115200);
    
    // GPIO Initialisation
    gpioSetup(GPIO);
    gpioMode(GPIO_MODES);

    // MIDI Initialisation
    mm_midi_setup();
}

void loop(){
    
    Algorithm* sustain = new Sustain(0, 0xFF);
    Algorithm* sustain2 = new Sustain(1, 0xFF);

    while(true){
        mm_midi_read();
        sustain->update();
        sustain2->update();
    }
}
