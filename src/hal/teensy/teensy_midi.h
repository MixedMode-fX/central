#ifndef MMMC_HAL_TEENSY_MIDI_H
#define MMMC_HAL_TEENSY_MIDI_H

#include "hal/teensy/teensy_includes.h"
#include "hal/imidi_out.h"
#include "hardware.h"

// DIN MIDI Settings
//
// MIDI_CREATE_CUSTOM_INSTANCE hands this one struct to both MidiInterface,
// which reads DefaultSettings (SysExMaxSize, running status, ...), and
// SerialMIDI, which reads DefaultSerialSettings (BaudRate). Inheriting from
// DefaultSerialSettings alone leaves SysExMaxSize undefined and the build
// fails, so inherit DefaultSettings and restate the baud rate here.
//
// MIDI Library 5.0.2 has no SerialFormat hook, so the TX inversion the DIN
// circuit needs is applied by reopening the UART in mm_midi_setup().
struct MidiSettings : public midi::DefaultSettings
{
    static const bool HandleNullVelocityNoteOnAsNoteOff = true;
    static const long BaudRate = 31250;
};

#define MIDI_SERIAL_FORMAT SERIAL_8N1_TXINV // TX needs inverting due to the circuit

#define ALL_MIDI_PORTS 0xFF

// Brings up every compiled-in transport. Call once from setup().
void mm_midi_setup();
// Pumps every compiled-in parser. Call once per main-loop pass.
void mm_midi_read();
// Sends to every port whose bit is set in `target`.
void mm_send(uint8_t target, uint8_t type, uint8_t data1, uint8_t data2, uint8_t channel);

// IMidiOut over the transports above.
class TeensyMidiOut : public IMidiOut {
    public:
        void send(uint8_t target, uint8_t type,
                  uint8_t d1, uint8_t d2, uint8_t channel) override {
            mm_send(target, type, d1, d2, channel);
        }
};

#endif
