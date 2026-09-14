#ifndef MMMC_HAL_TEENSY_LEDS_H
#define MMMC_HAL_TEENSY_LEDS_H

#include "hal/ileds.h"

// ILeds over GREEN_LED and RED_LED.
//
// Both pins are on a timer channel on every board map, so brightness is
// analogWrite() rather than digitalWrite() and the dim heartbeat StatusLeds
// asks for costs nothing extra. hardware.h asserts that; if a board turns up
// with the LEDs on pins without PWM, the only change needed here is to
// threshold `brightness` at digitalWrite level.
class TeensyLeds : public ILeds {
    public:
        // Claims both pins. Call once from setup().
        void begin();
        void set(uint8_t led, uint8_t brightness) override;
};

#endif
