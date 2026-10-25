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

// The wave is the pin's own PWM channel at half duty: FlexPWM or QuadTimer
// on every GPIO_PIN_n, so it is exact to the timer's clock and costs the
// main loop nothing. Two pins on one PWM submodule share its frequency
// (hardware.h), so the later of two tones on such a pair is what both play.
// The duty is written after the frequency because a frequency change
// rescales the period under it; 128 is half of analogWrite's default 8-bit
// range, which nothing in this firmware changes. Stopping hands the pin back
// to GPIO: pinMode() is what moves its mux off the timer.
void TeensyGpio::tone(uint8_t port, uint32_t millihertz){
    if (port >= GPIO_N) return;
    if (millihertz == 0){
        pinMode(pins_[port], OUTPUT);
        digitalWrite(pins_[port], LOW);
        return;
    }
    analogWriteFrequency(pins_[port], (float)millihertz / 1000.0f);
    analogWrite(pins_[port], 128);
}
