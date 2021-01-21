#ifndef GPIO_H
#define GPIO_H

#include <Arduino.h>

void gpioSetup(uint8_t pins[]);
void gpioMode(uint8_t pin, uint8_t mode);
void gpioDigitalWrite(uint8_t pin, uint8_t state);
void gpioAnalogWrite(uint8_t pin, uint8_t state);
uint8_t gpioDigitalRead(uint8_t pin);

void gpioMapMode(uint16_t pin_map, uint8_t mode);
void gpioMapDigitalWrite(uint16_t pin_map, uint8_t state);
void gpioMapAnalogWrite(uint16_t pin_map, uint8_t state);
void gpioMapDigitalRead(uint16_t pin_map, uint8_t *result);

// extern uint8_t *gpio;

#endif