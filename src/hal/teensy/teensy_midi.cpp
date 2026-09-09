#include "hal/teensy/teensy_midi.h"

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
#ifdef MMMC_USB_HOST
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
    midi1.turnThruOff();
    #endif

    #ifdef SERIAL_MIDI_2
    midi2.begin(MIDI_CHANNEL_OMNI);
    SERIAL_MIDI_2.begin(MidiSettings::BaudRate, MIDI_SERIAL_FORMAT);
    midi2.turnThruOff();
    #endif

    #ifdef SERIAL_MIDI_3
    midi3.begin(MIDI_CHANNEL_OMNI);
    SERIAL_MIDI_3.begin(MidiSettings::BaudRate, MIDI_SERIAL_FORMAT);
    midi3.turnThruOff();
    #endif

    #ifdef MMMC_USB_HOST
    mm_usb.begin();
    #endif
}


// Every parser is drained the same way: read one message, copy its fields,
// enqueue, repeat until the parser has nothing left. The three libraries
// (Teensy usbMIDI, the Arduino MIDI Library and USBHost_t36) expose the same
// read()/getType()/getChannel()/getData1()/getData2() shape, so one loop
// serves all five endpoints. Per-message callbacks would be five sets of
// seven handlers doing exactly this and no less work.
//
// Bounded per pass: a transport that receives faster than the loop runs must
// not be able to hold the loop inside one parser for ever.
static const uint8_t MAX_MESSAGES_PER_TRANSPORT = 16;

// SysEx is pulled out here rather than queued: MidiEvent has nowhere to put a
// variable-length payload, and the control plane must not share a buffer with
// musical traffic that a busy note stream could fill (#11).
#define MM_DRAIN(parser, source_bit) \
    for (uint8_t i = 0; i < MAX_MESSAGES_PER_TRANSPORT && parser.read(); i++){ \
        if ((uint8_t)parser.getType() == midi::SystemExclusive){ \
            sysex.deliver_sysex((source_bit), parser.getSysExArray(), \
                                (uint16_t)parser.getSysExArrayLength(), now_us); \
            continue; \
        } \
        queue.push((source_bit), MidiEvent{(uint8_t)parser.getType(), \
                                           (uint8_t)parser.getChannel(), \
                                           (uint8_t)parser.getData1(), \
                                           (uint8_t)parser.getData2()}); \
    }

void mm_midi_read(MidiInputQueue& queue, ISysexIn& sysex, uint32_t now_us){
    #ifdef MMMC_USB_HOST
    mm_usb.Task();
    MM_DRAIN(midi_hosted, mmMIDI_HOST_1)
    #endif

    #ifdef MIDI_INTERFACE
    // The four USB cables are one parser: the cable number says which of them
    // the message came in on.
    for (uint8_t i = 0; i < MAX_MESSAGES_PER_TRANSPORT && usbMIDI.read(); i++){
        const uint8_t cable = usbMIDI.getCable() & 0x03;
        const uint8_t source = (uint8_t)(mmMIDI_USB_0 << cable);
        if ((uint8_t)usbMIDI.getType() == usbMIDI.SystemExclusive){
            sysex.deliver_sysex(source, usbMIDI.getSysExArray(),
                                (uint16_t)usbMIDI.getSysExArrayLength(), now_us);
            continue;
        }
        queue.push(source,
                   MidiEvent{(uint8_t)usbMIDI.getType(),
                             (uint8_t)usbMIDI.getChannel(),
                             (uint8_t)usbMIDI.getData1(),
                             (uint8_t)usbMIDI.getData2()});
    }
    #endif

    #ifdef SERIAL_MIDI_1
    MM_DRAIN(midi1, mmMIDI_SERIAL_1)
    #endif

    #ifdef SERIAL_MIDI_2
    MM_DRAIN(midi2, mmMIDI_SERIAL_2)
    #endif
}

#undef MM_DRAIN

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

    #ifdef MMMC_USB_HOST
    if ((target & mmMIDI_HOST_1) == mmMIDI_HOST_1) midi_hosted.send(type, data1, data2, channel);
    #endif

}

// One complete message, F0 to F7 inclusive, to every port in the mask. Each
// library wants it slightly differently: the Arduino MIDI Library takes the
// array with a flag saying the framing bytes are already there.
void mm_send_sysex(uint8_t target, const uint8_t* data, uint16_t length){
    if (data == nullptr || length < 2) return;

    #ifdef MIDI_INTERFACE
    for (uint8_t cable = 0; cable < 4; cable++){
        if (target & (uint8_t)(mmMIDI_USB_0 << cable)) usbMIDI.sendSysEx(length, data, true, cable);
    }
    #endif

    #ifdef SERIAL_MIDI_1
    if (target & mmMIDI_SERIAL_1) midi1.sendSysEx(length, data, true);
    #endif

    #ifdef SERIAL_MIDI_2
    if (target & mmMIDI_SERIAL_2) midi2.sendSysEx(length, data, true);
    #endif

    #ifdef SERIAL_MIDI_3
    if (target & mmMIDI_SERIAL_3) midi3.sendSysEx(length, data, true);
    #endif

    #ifdef MMMC_USB_HOST
    if (target & mmMIDI_HOST_1) midi_hosted.sendSysEx(length, data, true);
    #endif
}
