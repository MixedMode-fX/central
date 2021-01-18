#ifndef __SUSTAIN_H_
#define __SUSTAIN_H_

#include <Arduino.h>

#include "gpio.h"
#include "mm_midi.h"
#include "algorithm.h"

class Sustain : public Algorithm{
    public:
        Sustain(uint8_t pedal_pin, uint8_t midi_outputs);
        ~Sustain();
        void update();

    private:
        uint8_t state = 1;
        uint8_t pedal_pin;
        uint8_t midi_outputs;
};

#endif