#ifndef MMMC_BOARD_TEENSY36_H
#define MMMC_BOARD_TEENSY36_H

// Pin and port map for the Teensy 3.6 board. Included by hardware.h, which
// holds the reasoning these numbers share with the other board and checks
// them; nothing else includes this file.
//
// **The jacks, the bus and the relays sit on the same pins as the 4.1.** Three
// things could not follow, because the chip cannot do them, and each is
// marked below: the MK66 has six UARTs where the i.MX RT has eight, and its
// pins 40-48 reach no ADC channel. Everything else is the same board with a
// slower part on it.

// The external clock input, as on the 4.1: pin 6 is free and interruptible.
#define SYNC_CLOCK 6
// The NVIC line SYNC_CLOCK's pin-change interrupt arrives on, so the clock can
// give it the interval timer's priority. This chip has one line per GPIO port
// rather than one for all of them, so it names the port SYNC_CLOCK sits on -
// pin 6 is PTD4 - and has to be changed with it.
#define SYNC_CLOCK_IRQ IRQ_PORTD

#define GPIO_PIN_1 2
#define GPIO_PIN_2 3
#define GPIO_PIN_3 4
#define GPIO_PIN_4 5
#define GPIO_PIN_5 9
#define GPIO_PIN_6 10
#define GPIO_PIN_7 22
#define GPIO_PIN_8 33

// Serial ports. Serial6 is LPUART0 here (pins 47/48) rather than the 4.1's
// pins 24/25; nothing in the firmware names those pins, only the port.
#define SERIAL_USB SerialUSB
#define SERIAL_UART Serial6

// MIDI ports.
//
// **MIDI 2 is Serial3, not the 4.1's Serial7, which this chip has not got.**
// Of the six UARTs here, Serial2 (pins 9/10) and Serial5 (pin 33) land on
// jacks, so the free ones are Serial1, Serial3, Serial4 and Serial6 - which is
// exactly the four ports the module needs.
#define SERIAL_MIDI_1 Serial1
#define SERIAL_MIDI_2 Serial3

// KeyMech connector.
//
// **KeyMech is Serial4, not the 4.1's Serial8, and reset moves off pin 31.**
// Serial4 is pins 31/32, so reset takes pin 29 - free, and next to boot.
#define SERIAL_KEYMECH Serial4
#define KEYMECH_BOOT 30
#define KEYMECH_RESET 29

// Relay control. SPI0 is on the same three pins as the 4.1's.
#define CS_RELAY 17
#define SPI_MOSI 11
#define SPI_MISO 12
#define SPI_SCK 13

// CV expansion.
//
// **The ADC input is pin 39 (A20), not the 4.1's pin 40.** Pins 40-48 on this
// chip are digital only, and analogRead() on one of them returns nothing that
// varies - which would quietly cost the boot entropy pool its noise source.
#define CS_CV_SHIFT 16
#define CS_DAC1 14
#define CS_DAC2 15
#define CV_ADC 39

// LEDs. FTM3 channels here rather than FLEXPWM, so brightness is still
// analogWrite().
#define GREEN_LED 36
#define RED_LED 37

#endif
