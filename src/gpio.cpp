#include "gpio.h"
#include "hardware.h"

uint8_t *gpio;

void gpioSetup(uint8_t pins[]){
    gpio = pins;
}

void gpioMode(uint8_t pin, uint8_t mode){
    pinMode(gpio[pin], mode);
}

void gpioMode(uint8_t modes[]){
    for(uint8_t j=0; j<8; j++){
        gpioMode(j, modes[j]);
        if(modes[j] == OUTPUT){
            gpioDigitalWrite(j, LOW);
        }
    }
}

void gpioDigitalWrite(uint8_t pin, uint8_t state){
    digitalWrite(gpio[pin], state);
}

void gpioAnalogWrite(uint8_t pin, uint8_t state){
    analogWrite(gpio[pin], state);
}

uint8_t gpioDigitalRead(uint8_t pin){
    return digitalRead(gpio[pin]);
}
