#ifndef HARDWARE_H
#define HARDWARE_H

#include <Arduino.h>

#define BOARD_ID 1

// EEPROM Addresses
#define BOARD_ID_ADDR 0 

/*
 *
 * SYNC
 * 
 */

#define SYNC_CLOCK 2

/*
 *
 * GPIO
 * 
 */

#define GPIO_N 8
#define GPIO_PIN_1 2
#define GPIO_PIN_2 3
#define GPIO_PIN_3 4
#define GPIO_PIN_4 5
#define GPIO_PIN_5 9
#define GPIO_PIN_6 10
#define GPIO_PIN_7 22
#define GPIO_PIN_8 33

#define GPIO_PINS GPIO_PIN_1,GPIO_PIN_2,GPIO_PIN_3,GPIO_PIN_4,GPIO_PIN_5,GPIO_PIN_6,GPIO_PIN_7,GPIO_PIN_8

#define ALL_GPIO_MAP 0xFF

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