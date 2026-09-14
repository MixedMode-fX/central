#ifndef HARDWARE_H
#define HARDWARE_H

#include "config.h"
#include "hal/teensy/teensy_includes.h"

// The hardware surface: one board map, selected by the board the framework
// says it is compiling for, plus the reasoning and the checks the maps share.
// Only src/hal/teensy/ and main.cpp include this file.
//
// A new board is a new header under src/board/ and a line here. Every name a
// map has to define is used below or by src/hal/teensy/, so a map that misses
// one does not link.
#if defined(ARDUINO_TEENSY41)
#include "board/teensy41.h"
#elif defined(ARDUINO_TEENSY36)
#include "board/teensy36.h"
#else
#error "no board map for this target - add one under src/board/"
#endif

/*
 *
 * GPIO
 *
 */

#define GPIO_PINS GPIO_PIN_1,GPIO_PIN_2,GPIO_PIN_3,GPIO_PIN_4,GPIO_PIN_5,GPIO_PIN_6,GPIO_PIN_7,GPIO_PIN_8

/*
 * Gate input polarity.
 *
 * Each gate input is read through the pin's internal pull-up, with the input
 * stage pulling the pin LOW while a gate is present at the jack. The pin
 * therefore reads the inverse of the jack, and an unpatched input reads HIGH.
 * TeensyGpio::read() normalises this in one place: it returns GPIO_HIGH when
 * a gate is present and GPIO_LOW otherwise, so an unpatched input is 0 - the
 * identity of OR and XOR. Set to 0 if a board revision delivers true polarity.
 *
 * Assumption to verify against the schematic of the assembled board.
 */
#define GATE_INPUT_ACTIVE_LOW 1

/* Ports start as inputs at boot and stay that way until an algorithm claims
 * one as an output; nothing drives a port before then. */

#ifdef __cplusplus
namespace hardware_checks {
    constexpr uint8_t gpio_pins[GPIO_N] = {GPIO_PINS};
    constexpr bool sync_clock_is_free(uint8_t i = 0) {
        return i >= GPIO_N ? true : (gpio_pins[i] != SYNC_CLOCK && sync_clock_is_free(i + 1));
    }
    static_assert(sync_clock_is_free(), "SYNC_CLOCK collides with a GPIO_PIN_n");

    // A pin the chip has not got is a pin nothing on the map may name. The
    // core reports its own count, so a map written for a bigger part fails
    // here rather than on the bench.
    constexpr uint8_t named_pins[] = {
        GPIO_PINS, SYNC_CLOCK, KEYMECH_BOOT, KEYMECH_RESET,
        CS_RELAY, SPI_MOSI, SPI_MISO, SPI_SCK,
        CS_CV_SHIFT, CS_DAC1, CS_DAC2, CV_ADC, GREEN_LED, RED_LED,
    };
    constexpr bool pins_exist(uint8_t i = 0) {
        return i >= sizeof named_pins ? true
             : (named_pins[i] < CORE_NUM_TOTAL_PINS && pins_exist(i + 1));
    }
    static_assert(pins_exist(), "a pin on the board map is not on this chip");

    // Both LEDs are driven with analogWrite() for the dim heartbeat
    // (hal/teensy/teensy_leds.h), so both have to be on a timer channel.
    static_assert(digitalPinHasPWM(GREEN_LED) && digitalPinHasPWM(RED_LED),
                  "an LED pin has no PWM on this chip");

    // Not checked here, because the core publishes no compile-time map of it:
    // CV_ADC is read with analogRead() for boot entropy (main.cpp), so it has
    // to reach an ADC channel, and only the part's datasheet says which pins
    // do. On the MK66, for one, pins 40-48 are digital only.
}
#endif

/*
 *
 * Serial ports
 *
 */

#define SERIAL_BAUD_RATE 115200

#endif
