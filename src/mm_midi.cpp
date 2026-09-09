#include "mm_midi.h"

#ifdef SERIAL_MIDI_1
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, SERIAL_MIDI_1, midi1, MidiSettings);
#endif
#ifdef SERIAL_MIDI_2
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, SERIAL_MIDI_2, midi2, MidiSettings);
#endif
#ifdef SERIAL_MIDI_3
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, SERIAL_MIDI_3, midi3, MidiSettings);
#endif


// USB Host MIDI
#ifdef USB_HOST_TEENSY36_
USBHost mm_usb;
USBHub hub1(mm_usb);
USBHub hub2(mm_usb);
USBHub hub3(mm_usb);
MIDIDevice midi_hosted(mm_usb);
#endif

void mm_midi_setup(){
    #ifdef SERIAL_MIDI_1
    midi1.begin(MIDI_CHANNEL_OMNI);
    SERIAL_MIDI_1.begin(MidiSettings::BaudRate, MIDI_SERIAL_FORMAT);
    // midi1.turnThruOff();
    #endif

    #ifdef SERIAL_MIDI_2
    midi2.begin(MIDI_CHANNEL_OMNI);
    SERIAL_MIDI_2.begin(MidiSettings::BaudRate, MIDI_SERIAL_FORMAT);
    // midi2.turnThruOff();
    #endif

    #ifdef SERIAL_MIDI_3
    midi3.begin(MIDI_CHANNEL_OMNI);
    SERIAL_MIDI_3.begin(MidiSettings::BaudRate, MIDI_SERIAL_FORMAT);
    midi3.turnThruOff();
    #endif

    #ifdef USB_HOST_TEENSY36_
    mm_usb.begin();
    #endif
}


void mm_midi_read(){
    #ifdef USB_HOST_TEENSY36_
    mm_usb.Task();
    midi_hosted.read();
    #endif

    #ifdef MIDI_INTERFACE
    usbMIDI.read();
    #endif

    #ifdef SERIAL_MIDI_1
    midi1.read();
    #endif

    #ifdef SERIAL_MIDI_2
    midi2.read();
    #endif
}

void mm_send(uint8_t target, uint8_t type, uint8_t data1, uint8_t data2, uint8_t channel){
    // Send to one or many MIDI port

    #ifdef MIDI_INTERFACE
    if ((target & mmMIDI_USB_0) == mmMIDI_USB_0) usbMIDI.send(type, data1, data2, channel, 0);
    if ((target & mmMIDI_USB_1) == mmMIDI_USB_1) usbMIDI.send(type, data1, data2, channel, 1);
    if ((target & mmMIDI_USB_2) == mmMIDI_USB_2) usbMIDI.send(type, data1, data2, channel, 2);
    if ((target & mmMIDI_USB_3) == mmMIDI_USB_3) usbMIDI.send(type, data1, data2, channel, 3);
    #endif

    #ifdef SERIAL_MIDI_1
    if ((target & mmMIDI_SERIAL_1) == mmMIDI_SERIAL_1) midi1.send((midi::MidiType)type, data1, data2, channel);
    #endif 

    #ifdef SERIAL_MIDI_2
    if ((target & mmMIDI_SERIAL_2) == mmMIDI_SERIAL_2) midi2.send((midi::MidiType)type, data1, data2, channel);
    #endif 

    #ifdef SERIAL_MIDI_3
    if ((target & mmMIDI_SERIAL_3) == mmMIDI_SERIAL_3) midi3.send((midi::MidiType)type, data1, data2, channel);
    #endif

    #ifdef USB_HOST_TEENSY36_
    if ((target & mmMIDI_HOST_1) == mmMIDI_HOST_1) midi_hosted.send(type, data1, data2, channel);
    #endif

}
