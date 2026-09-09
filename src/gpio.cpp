#include "gpio.h"
#include "hardware.h"

uint8_t *gpio;

void gpioSetup(uint8_t pins[]){
    gpio = pins;
}

void gpioMode(uint8_t pin, uint8_t mode){
    if(pin < GPIO_N){
        pinMode(gpio[pin], mode);
    }
}

void gpioMapMode(uint16_t pin_map, uint8_t mode){
    uint8_t mask = 0;
    for (uint8_t pin = 0; pin < GPIO_N; pin++){
        mask = 1 << pin;
        if (mask > pin_map) break;
        if ((pin_map & mask) == mask){
            pinMode(gpio[pin], mode);
        } 
        
    }
}

void gpioDigitalWrite(uint8_t pin, uint8_t state){
    digitalWrite(gpio[pin], state);
}

void gpioMapDigitalWrite(uint16_t pin_map, uint8_t state){
    uint8_t mask = 0;
    for (uint8_t pin = 0; pin < GPIO_N; pin++){
        mask = 1 << pin;
        if (mask > pin_map) break;
        if ((pin_map & mask) == mask){
            gpioDigitalWrite(pin, state);
        }
    }
}


void gpioAnalogWrite(uint8_t pin, uint8_t state){
    analogWrite(gpio[pin], state);
}

void gpioMapAnalogWrite(uint16_t pin_map, uint8_t state){
    uint8_t mask = 0;
    for (uint8_t pin = 0; pin < GPIO_N; pin++){
        mask = 1 << pin;
        if (mask > pin_map) break;
        if ((pin_map & mask) == mask){
            gpioAnalogWrite(pin, state);
        }
    }
}

uint8_t gpioDigitalRead(uint8_t pin){
    return digitalRead(gpio[pin]);
}

void gpioMapDigitalRead(uint16_t pin_map, uint8_t *result){
    uint8_t mask = 0;
    for (uint8_t i = 0; i < GPIO_N; i++){
        mask = 1 << i;
        if (mask > pin_map) break;
        if ((pin_map & mask) == mask){
            uint8_t r = gpioDigitalRead(i);
            result[i] = r;
        }
    }
}