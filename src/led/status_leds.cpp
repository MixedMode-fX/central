#include "led/status_leds.h"

StatusLeds::StatusLeds(ILeds& driver) :
    leds(driver),
    beat_until_us(0), error_until_us(0), identify_until_us(0), identify_start_us(0),
    error_count(0), last(), defaults(false), clock_running(false)
{
    // Both LEDs start off, and are written so, rather than assumed: `last`
    // cannot hold a "never written" sentinel because every uint8_t is a
    // valid brightness - BRIGHT is 0xFF.
    for (uint8_t i = 0; i < LED_COUNT; i++){
        last[i] = 0;
        leds.set(i, 0);
    }
}

void StatusLeds::beat(uint32_t now_us){
    beat_until_us = now_us + BEAT_FLASH_US;
}

void StatusLeds::error(uint32_t now_us){
    error_count++;
    error_until_us = now_us + ERROR_FLASH_US;
}

void StatusLeds::identify(uint32_t now_us){
    identify_start_us = now_us;
    identify_until_us = now_us + (uint32_t)IDENTIFY_BLINKS * IDENTIFY_STEP_US;
}

// Only writes a pin when the level actually changed, so a driver that does
// analogWrite() is not asked to reprogram a PWM channel a thousand times a
// second for no reason.
void StatusLeds::write(uint8_t led, uint8_t brightness){
    if (led >= LED_COUNT || last[led] == brightness) return;
    last[led] = brightness;
    leds.set(led, brightness);
}

void StatusLeds::service(uint32_t now_us){
    // Identity wins both LEDs while it runs: a user hunting for which of two
    // modules answered should not have to read past a beat flash.
    if ((int32_t)(identify_until_us - now_us) > 0){
        const uint32_t step = (uint32_t)(now_us - identify_start_us) / IDENTIFY_STEP_US;
        const bool green_phase = (step & 1u) == 0u;
        write(LED_GREEN, green_phase ? BRIGHT : OFF);
        write(LED_RED, green_phase ? OFF : BRIGHT);
        return;
    }

    // Green: the beat flash while a clock is running, a dim heartbeat when
    // one is not.
    if ((int32_t)(beat_until_us - now_us) > 0){
        write(LED_GREEN, BRIGHT);
    } else if (clock_running){
        write(LED_GREEN, OFF);
    } else {
        const uint32_t phase = now_us % HEARTBEAT_PERIOD_US;
        write(LED_GREEN, phase < HEARTBEAT_ON_US ? DIM : OFF);
    }

    // Red: solid while the module is running the defaults because it had no
    // valid patch - that is a state, not an event, and it outranks a flash.
    if (defaults){
        write(LED_RED, BRIGHT);
    } else if ((int32_t)(error_until_us - now_us) > 0){
        write(LED_RED, BRIGHT);
    } else {
        write(LED_RED, OFF);
    }
}
