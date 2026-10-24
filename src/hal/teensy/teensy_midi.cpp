#include "hal/teensy/teensy_midi.h"

#ifdef SERIAL_MIDI_1
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, SERIAL_MIDI_1, midi1, MidiSettings);
#endif
#ifdef SERIAL_MIDI_2
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, SERIAL_MIDI_2, midi2, MidiSettings);
#endif


// USB Host MIDI
//
// Three hubs deep or wide, and one MIDIDevice per MIDI interface plugged in
// anywhere below them: USBHost_t36 hands each new interface to the first
// driver that is free.
#ifdef MMMC_USB_HOST
USBHost mm_usb;
USBHub hub1(mm_usb);
USBHub hub2(mm_usb);
USBHub hub3(mm_usb);
MIDIDevice host_midi_0(mm_usb);
MIDIDevice host_midi_1(mm_usb);
MIDIDevice host_midi_2(mm_usb);
MIDIDevice host_midi_3(mm_usb);
MIDIDevice host_midi_4(mm_usb);
MIDIDevice host_midi_5(mm_usb);
static MIDIDevice* const host_midi[] = {&host_midi_0, &host_midi_1, &host_midi_2,
                                        &host_midi_3, &host_midi_4, &host_midi_5};
static_assert(sizeof host_midi / sizeof host_midi[0] == N_HOST_MIDI_DEVICES,
              "one MIDIDevice per N_HOST_MIDI_DEVICES");

// Focusrite-Novation's vendor id, and the product ids of the three models
// the surface drives (surface/launchpad.h).
static const uint16_t NOVATION_VID = 0x1235;
static const uint16_t LAUNCHPAD_PIDS[] = {0x0103 /* X */, 0x0113 /* Mini MK3 */, 0x0123 /* Pro MK3 */};

// Which of the socket's two cables a hosted device is (hal/midi_types.h):
// a Launchpad is the surface's and nothing else's.
static uint8_t host_cable(MIDIDevice& device){
    if (device.idVendor() == NOVATION_VID){
        for (uint16_t pid : LAUNCHPAD_PIDS){
            if (device.idProduct() == pid) return mmMIDI_HOST_LAUNCHPAD;
        }
    }
    return mmMIDI_HOST_1;
}
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

    #ifdef MMMC_USB_HOST
    mm_usb.begin();
    #endif
}


// Every parser is drained the same way: read one message, copy its fields,
// enqueue, repeat until the parser has nothing left. The three libraries
// (Teensy usbMIDI, the Arduino MIDI Library and USBHost_t36) expose the same
// read()/getType()/getChannel()/getData1()/getData2() shape, so one loop
// serves every endpoint. Per-message callbacks would be five sets of
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
    for (MIDIDevice* device : host_midi){
        if (!*device) continue;
        const uint8_t cable = host_cable(*device);
        MM_DRAIN((*device), cable)
    }
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

    #ifdef MMMC_USB_HOST
    if (target & (mmMIDI_HOST_LAUNCHPAD | mmMIDI_HOST_1)){
        for (MIDIDevice* device : host_midi){
            if (*device && (target & host_cable(*device))) device->send(type, data1, data2, channel);
        }
    }
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

    #ifdef MMMC_USB_HOST
    if (target & (mmMIDI_HOST_LAUNCHPAD | mmMIDI_HOST_1)){
        for (MIDIDevice* device : host_midi){
            if (*device && (target & host_cable(*device))) device->sendSysEx(length, data, true);
        }
    }
    #endif
}
