#include "hal/teensy/teensy_gpio.h"
#include "hal/teensy/teensy_includes.h"
#include "hardware.h"

TeensyGpio::TeensyGpio(const uint8_t (&pins)[GPIO_N]) : pins_() {
    for (uint8_t i = 0; i < GPIO_N; i++) pins_[i] = pins[i];
}

static uint8_t arduino_mode(uint8_t mode){
    switch (mode){
        case GPIO_MODE_INPUT:        return INPUT;
        case GPIO_MODE_INPUT_PULLUP: return INPUT_PULLUP;
        default:                     return OUTPUT;
    }
}

// Every accessor bounds-checks the port: an out-of-range index must never
// read past the pin table and drive whatever Teensy pin that byte names.

void TeensyGpio::mode(uint8_t port, uint8_t mode){
    if (port >= GPIO_N) return;
    pinMode(pins_[port], arduino_mode(mode));
}

void TeensyGpio::write(uint8_t port, uint8_t state){
    if (port >= GPIO_N) return;
    digitalWrite(pins_[port], state ? HIGH : LOW);
}

uint8_t TeensyGpio::read(uint8_t port){
    if (port >= GPIO_N) return GPIO_LOW;
    const bool pin_high = digitalRead(pins_[port]) != LOW;
#if GATE_INPUT_ACTIVE_LOW
    return pin_high ? GPIO_LOW : GPIO_HIGH;   // see hardware.h
#else
    return pin_high ? GPIO_HIGH : GPIO_LOW;
#endif
}
