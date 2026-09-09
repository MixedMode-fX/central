#ifndef MMMC_TEENSY_INCLUDES_H
#define MMMC_TEENSY_INCLUDES_H

// Single entry point for the Teensy core and the third-party libraries.
//
// Our own sources are compiled with -Wall -Wextra -Weffc++ -Wshadow -Werror
// (see build_src_flags in platformio.ini). The framework headers are not held
// to that standard, so they are included here with those diagnostics silenced
// for the duration of the include only. Every file that needs the Arduino
// core, the MIDI library or USBHost_t36 includes this header, never the
// libraries directly.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include <Arduino.h>
#include <MIDI.h>
#ifdef MMMC_USB_HOST
#include <USBHost_t36.h>
#endif

#pragma GCC diagnostic pop

#endif
