#ifndef HARDWARE_H
#define HARDWARE_H

#include "config.h"
#include "hal/teensy/teensy_includes.h"

#define BOARD_ID 1

// EEPROM Addresses
#define BOARD_ID_ADDR 0 

/*
 *
 * SYNC
 *
 * The external clock input. It used to be pin 2, which is also GPIO_PIN_1:
 * boot drove that pin as an output while an external clock could be driving
 * into it. Pin 6 is free on this board revision and supports interrupts.
 * The static_assert below keeps it off the GPIO table.
 */

#define SYNC_CLOCK 6

/*
 *
 * GPIO
 * 
 */

#define GPIO_PIN_1 2
#define GPIO_PIN_2 3
#define GPIO_PIN_3 4
#define GPIO_PIN_4 5
#define GPIO_PIN_5 9
#define GPIO_PIN_6 10
#define GPIO_PIN_7 22
#define GPIO_PIN_8 33

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
}
#endif

/*
 *
 * Serial ports
 * 
 */

#define SERIAL_USB SerialUSB
#define SERIAL_UART Serial6
#define SERIAL_BAUD_RATE 115200

/*
 *
 * MIDI Ports
 * 
 */

#define SERIAL_MIDI_1 Serial1
#define SERIAL_MIDI_2 Serial7

/*
 *
 * KeyMech Connector
 * 
 */

#define SERIAL_KEYMECH Serial8
#define KEYMECH_BOOT 30
#define KEYMECH_RESET 31

/*
 *
 * Relay control
 * 
 */

#define CS_RELAY 17
#define SPI_MOSI 11
#define SPI_MISO 12
#define SPI_SCK 13

/*
 *
 * CV Expansion
 * 
 */

#define CS_CV_SHIFT 16
#define CS_DAC1 14
#define CS_DAC2 15
#define CV_ADC 40



/*
 *
 * LEDS
 * 
 */

#define GREEN_LED 36
#define RED_LED 37

#endif