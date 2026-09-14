#ifndef MMMC_BOARD_TEENSY41_H
#define MMMC_BOARD_TEENSY41_H

// Pin and port map for the Teensy 4.1 board. Included by hardware.h, which
// holds the reasoning these numbers share with the other board and checks
// them; nothing else includes this file.

// The external clock input. It used to be pin 2, which is also GPIO_PIN_1:
// boot drove that pin as an output while an external clock could be driving
// into it. Pin 6 is free on this board revision and supports interrupts.
#define SYNC_CLOCK 6
// The NVIC line SYNC_CLOCK's pin-change interrupt arrives on, so the clock
// can give it the interval timer's priority. One line serves every GPIO port
// on this chip, so it holds whatever SYNC_CLOCK becomes.
#define SYNC_CLOCK_IRQ IRQ_GPIO6789

#define GPIO_PIN_1 2
#define GPIO_PIN_2 3
#define GPIO_PIN_3 4
#define GPIO_PIN_4 5
#define GPIO_PIN_5 9
#define GPIO_PIN_6 10
#define GPIO_PIN_7 22
#define GPIO_PIN_8 33

// Serial ports.
#define SERIAL_USB SerialUSB
#define SERIAL_UART Serial6

// MIDI ports.
#define SERIAL_MIDI_1 Serial1
#define SERIAL_MIDI_2 Serial7

// KeyMech connector.
#define SERIAL_KEYMECH Serial8
#define KEYMECH_BOOT 30
#define KEYMECH_RESET 31

// Relay control.
#define CS_RELAY 17
#define SPI_MOSI 11
#define SPI_MISO 12
#define SPI_SCK 13

// CV expansion.
#define CS_CV_SHIFT 16
#define CS_DAC1 14
#define CS_DAC2 15
#define CV_ADC 40

// LEDs. Both are FLEXPWM outputs, so brightness is analogWrite().
#define GREEN_LED 36
#define RED_LED 37

#endif
