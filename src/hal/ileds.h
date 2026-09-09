#ifndef MMMC_HAL_ILEDS_H
#define MMMC_HAL_ILEDS_H

#include <stdint.h>

// The two status LEDs (#7). `brightness` is 0..255; a driver without PWM
// treats anything non-zero as on.
//
// Pins 36 and 37 on a Teensy 4.1 are both FLEXPWM-capable, so brightness is
// a second dimension for free - and it is worth having, because a dim
// heartbeat against a bright beat flash reads as two different things at a
// glance where on-versus-off does not.
enum StatusLed : uint8_t {
    LED_GREEN = 0,
    LED_RED   = 1,
    LED_COUNT = 2,
};

struct ILeds {
    virtual ~ILeds() = default;
    virtual void set(uint8_t led, uint8_t brightness) = 0;
};

#endif
