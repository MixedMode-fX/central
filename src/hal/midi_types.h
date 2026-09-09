#ifndef MMMC_HAL_MIDI_TYPES_H
#define MMMC_HAL_MIDI_TYPES_H

#include <stdint.h>

// MIDI status bytes, framework-free. Values match midi::MidiType from the
// Arduino MIDI Library so the Teensy transport can cast straight through.
enum MidiType : uint8_t {
    MIDI_NOTE_OFF           = 0x80,
    MIDI_NOTE_ON            = 0x90,
    MIDI_AFTERTOUCH_POLY    = 0xA0,
    MIDI_CONTROL_CHANGE     = 0xB0,
    MIDI_PROGRAM_CHANGE     = 0xC0,
    MIDI_AFTERTOUCH_CHANNEL = 0xD0,
    MIDI_PITCH_BEND         = 0xE0,
    MIDI_CLOCK              = 0xF8,
    MIDI_START              = 0xFA,
    MIDI_CONTINUE           = 0xFB,
    MIDI_STOP               = 0xFC,
};

// Logical MIDI endpoints, one bit each. A target mask is any OR of these.
// Which of them are actually compiled in is the transport's business
// (see hal/teensy/teensy_midi.h).
enum MidiPort : uint8_t {
    mmMIDI_USB_0    = 0x01,
    mmMIDI_USB_1    = 0x02,
    mmMIDI_USB_2    = 0x04,
    mmMIDI_USB_3    = 0x08,
    mmMIDI_SERIAL_1 = 0x10,
    mmMIDI_SERIAL_2 = 0x20,
    mmMIDI_SERIAL_3 = 0x40,
    mmMIDI_HOST_1   = 0x80,
};

// Cable 3 of the USB device port is reserved for control traffic (#11), so a
// patch transfer never mixes with musical MIDI and a busy note stream cannot
// starve a dump. Nothing musical is ever routed to it, and a patch cannot
// take it away from the protocol.
constexpr uint8_t MIDI_CONTROL_PORT = mmMIDI_USB_3;

// Every logical port that carries music: all of them but the control cable.
// A transport ignores the bits it has no port for, so this is safe in any
// build (hal/teensy/teensy_midi.h derives the narrower ALL_MIDI_PORTS from
// what is actually compiled in).
constexpr uint8_t MIDI_MUSICAL_PORTS = (uint8_t)(0xFFu & (uint8_t)~MIDI_CONTROL_PORT);

#endif
