#ifndef __MM_MIDI_H_
#define __MM_MIDI_H_

#include <Arduino.h>
#include <MIDI.h>
#include <USBHost_t36.h>

#include "hardware.h"


// DIN MIDI Settings
struct MidiSettings : public midi::DefaultSerialSettings
{
    static const bool HandleNullVelocityNoteOnAsNoteOff = true;
    static const uint16_t SerialFormat = SERIAL_8N1_TXINV; // TX needs inverting due to the circuit
};




enum MidiPorts{
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

void mm_midi_setup();
void mm_midi_read();
void mm_send(uint8_t target, uint8_t type, uint8_t data1, uint8_t data2, uint8_t channel);


#endif