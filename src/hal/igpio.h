#ifndef MMMC_HAL_IGPIO_H
#define MMMC_HAL_IGPIO_H

#include <stdint.h>

// Pin modes and levels, independent of the Arduino constants so that code
// built against this interface never needs Arduino.h.
enum GpioMode : uint8_t {
    GPIO_MODE_INPUT = 0,
    GPIO_MODE_INPUT_PULLUP = 1,
    GPIO_MODE_OUTPUT = 2,
};

enum GpioLevel : uint8_t {
    GPIO_LOW = 0,
    GPIO_HIGH = 1,
};

// The one seam between the firmware logic and the front-panel ports.
// `port` is a logical port index (0 .. GPIO_N-1), never a Teensy pin number.
struct IGpio {
    virtual ~IGpio() = default;
    virtual void mode(uint8_t port, uint8_t mode) = 0;
    virtual void write(uint8_t port, uint8_t state) = 0;
    virtual uint8_t read(uint8_t port) = 0;
};

#endif
