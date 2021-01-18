#include <Arduino.h>

#include "hardware.h"
#include "gpio.h"
#include "mm_midi.h"

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
    mm_midi_read();
}
