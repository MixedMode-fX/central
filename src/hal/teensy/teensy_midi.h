#ifndef MMMC_HAL_TEENSY_MIDI_H
#define MMMC_HAL_TEENSY_MIDI_H

#include "hal/teensy/teensy_includes.h"
#include "hal/imidi_out.h"
#include "hal/isysex_in.h"
#include "midi/midi_queue.h"
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

// Every port this build actually has a transport for. Derived, not
// hardcoded: a bit is only set when the port behind it is compiled in.
constexpr uint8_t ALL_MIDI_PORTS = 0
#ifdef MIDI_INTERFACE
    | mmMIDI_USB_0 | mmMIDI_USB_1 | mmMIDI_USB_2 | mmMIDI_USB_3
#endif
#ifdef SERIAL_MIDI_1
    | mmMIDI_SERIAL_1
#endif
#ifdef SERIAL_MIDI_2
    | mmMIDI_SERIAL_2
#endif
#ifdef SERIAL_MIDI_3
    | mmMIDI_SERIAL_3
#endif
#ifdef MMMC_USB_HOST
    | mmMIDI_HOST_1
#endif
    ;

// Brings up every compiled-in transport. Call once from setup().
//
// Library soft-thru is turned off on both DIN ports: an input echoed straight
// to an output is a patch, not a default. A MidiInPort and a MidiOutPort on a
// shared note bus give the same behaviour when somebody asks for it (#5).
void mm_midi_setup();
// Pumps every compiled-in parser and enqueues whatever it produced, tagged
// with the transport it arrived on. Nothing else happens here: the pass that
// follows is where messages are dispatched. Call once per main-loop pass.
//
// SysEx never reaches the queue: it is control-plane traffic, not a bus
// event, so a complete message goes straight to `sysex` (#11), stamped with
// the pass's `now_us` like every other control-plane call.
void mm_midi_read(MidiInputQueue& queue, ISysexIn& sysex, uint32_t now_us);
// Sends to every port whose bit is set in `target`.
void mm_send(uint8_t target, uint8_t type, uint8_t data1, uint8_t data2, uint8_t channel);
// Sends one complete SysEx message, F0 to F7 inclusive.
void mm_send_sysex(uint8_t target, const uint8_t* data, uint16_t length);

// IMidiOut over the transports above.
class TeensyMidiOut : public IMidiOut {
    public:
        void send(uint8_t target, uint8_t type,
                  uint8_t d1, uint8_t d2, uint8_t channel) override {
            mm_send(target, type, d1, d2, channel);
        }
        void send_sysex(uint8_t target, const uint8_t* data, uint16_t length) override {
            mm_send_sysex(target, data, length);
        }
};

#endif
