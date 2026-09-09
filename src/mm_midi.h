#ifndef __MM_MIDI_H_
#define __MM_MIDI_H_

#include "hal/teensy/teensy_includes.h"

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




enum MidiPort   {
    #ifdef MIDI_INTERFACE
    mmMIDI_USB_0 = 0x1,
    mmMIDI_USB_1 = 0x2,
    mmMIDI_USB_2 = 0x4,
    mmMIDI_USB_3 = 0x8,
    #endif 

    #ifdef SERIAL_MIDI_1
    mmMIDI_SERIAL_1 = 0x10,
    #endif

    #ifdef SERIAL_MIDI_2
    mmMIDI_SERIAL_2 = 0x20,
    #endif

    #ifdef SERIAL_MIDI_3
    mmMIDI_SERIAL_3 = 0x40,
    #endif


    #ifdef USB_HOST_TEENSY36_
    mmMIDI_HOST_1 = 0x80,
    #endif
};

#define ALL_MIDI_PORTS 0xFF

void mm_midi_setup();
void mm_midi_read();
void mm_send(uint8_t target, uint8_t type, uint8_t data1, uint8_t data2, uint8_t channel);


#endif