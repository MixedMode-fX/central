#ifndef GPIO_H
#define GPIO_H

#include <Arduino.h>

void gpioSetup(uint8_t pins[]);
void gpioMode(uint8_t pin, uint8_t mode);
void gpioMode(uint8_t modes[]);
void gpioDigitalWrite(uint8_t pin, uint8_t state);
void gpioAnalogWrite(uint8_t pin, uint8_t state);
uint8_t gpioDigitalRead(uint8_t pin);
extern uint8_t *gpio;

#endif