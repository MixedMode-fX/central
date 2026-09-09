#include "hal/teensy/teensy_gpio.h"
#include "hal/teensy/teensy_includes.h"

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

void TeensyGpio::mode(uint8_t port, uint8_t mode){
    if (port < GPIO_N){
        pinMode(pins_[port], arduino_mode(mode));
    }
}

void TeensyGpio::write(uint8_t port, uint8_t state){
    digitalWrite(pins_[port], state ? HIGH : LOW);
}

uint8_t TeensyGpio::read(uint8_t port){
    return digitalRead(pins_[port]) ? GPIO_HIGH : GPIO_LOW;
}
