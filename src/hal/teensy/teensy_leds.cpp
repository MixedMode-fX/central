#include "hal/teensy/teensy_includes.h"
#include "hal/teensy/teensy_leds.h"
#include "hardware.h"

static const uint8_t LED_PIN[LED_COUNT] = {GREEN_LED, RED_LED};

void TeensyLeds::begin(){
    for (uint8_t i = 0; i < LED_COUNT; i++){
        pinMode(LED_PIN[i], OUTPUT);
        analogWrite(LED_PIN[i], 0);
    }
}

void TeensyLeds::set(uint8_t led, uint8_t brightness){
    if (led >= LED_COUNT) return;
    analogWrite(LED_PIN[led], brightness);
}
