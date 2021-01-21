#include <Arduino.h>

#include "hardware.h"
#include "gpio.h"
#include "mm_midi.h"
#include "sustain.h"
#include "logic/not.h"
#include "logic/and.h"
#include "logic/or.h"
#include "logic/xor.h"

uint8_t GPIO[GPIO_N] = {GPIO_PINS};

void setup(){
    Serial.begin(115200);
    
    // GPIO Initialisation
    gpioSetup(GPIO);
    gpioMapMode(ALL_GPIO_MAP, OUTPUT);
    gpioMapDigitalWrite(ALL_GPIO_MAP, LOW); 

    // MIDI Initialisation
    mm_midi_setup();


}

void loop(){    
    Algorithm* sustain = new Sustain(       0, ALL_MIDI_PORTS,  0b00000001, 0);
    Algorithm* logic_not = new LogicNot(    0, 0,               0b00000001, 0b00011110);
    Algorithm* logic_and = new LogicNXOR(    0, 0,               0b11000000, 0b00100000);
    while(true){
        mm_midi_read();
        sustain->update();
        logic_not->update();
        logic_and->update();
    }
}
