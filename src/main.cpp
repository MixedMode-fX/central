#include <Arduino.h>

#include "hardware.h"
#include "gpio.h"
#include "mm_midi.h"
#include "version.h"

#include "algorithm/switch/sustain.h"
#include "algorithm/logic/gates.h"
// #include "algorithm/midi/priority.h"

uint8_t GPIO[GPIO_N] = {GPIO_PINS};

void setup(){
    Serial.begin(115200);
    Serial.println("MMMC " MMMC_BUILD);
    
    // GPIO Initialisation
    gpioSetup(GPIO);
    gpioMapMode(ALL_GPIO_MAP, OUTPUT);
    gpioMapDigitalWrite(ALL_GPIO_MAP, LOW); 

    // MIDI Initialisation
    mm_midi_setup();


}

void loop(){    
    Algorithm* sustain = new Sustain(       0, ALL_MIDI_PORTS,  0b10000000, 0);
    // Algorithm* logic_not = new LogicNot(    0, 0,               0b00000001, 0b00011110);
    // Algorithm* logic_and = new LogicNAND(    0, 0,               0b11000000, 0b00100000);
    while(true){
        mm_midi_read();
        // logic_not->update();
        // logic_and->update();
        sustain->update();
    }
}
