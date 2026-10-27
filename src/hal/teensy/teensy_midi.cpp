#include "hal/teensy/teensy_midi.h"
#include <stdio.h>
#include <string.h>
#include "hal/teensy/teensy_audio.h"

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
namespace {

// Every device the socket enumerates, whether a driver wants it or not, for
// the console's `usb` (hal/iusb_host.h). USBHost_t36 offers each new device
// whole to every free driver before any interface is offered; this one
// writes down what it was offered and refuses it, so it stays free and sees
// the next. A driver that never claims is never told of a disconnect, so this
// is a history, not what is plugged in now: the drivers say that. Defined
// before the hubs, which take the whole device, so it is asked first.
//
// The -Wnon-virtual-dtor waiver is teensy_audio.cpp's, for the same reason.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
class EnumerationLog final : public USBDriver {
public:
    static constexpr uint8_t ENTRIES = 8;
    static constexpr uint8_t PRODUCT_MAX = 31;
    struct Entry {
        uint16_t vid, pid;
        bool audio;            // it offered an audio function
        char product[PRODUCT_MAX + 1];
    };

    EnumerationLog() : entries(), count(0) { driver_ready_for_device(this); }

    // Seen since boot, of which the last ENTRIES are kept.
    uint32_t seen() const { return count; }
    const Entry& entry(uint32_t n) const { return entries[n % ENTRIES]; }

protected:
    // In the USB interrupt, during enumeration, after the strings are read.
    bool claim(Device_t* dev, int type, const uint8_t* descriptors, uint32_t len) override {
        if (type != 0) return false;
        Entry& e = entries[count % ENTRIES];
        e.vid = dev->idVendor;
        e.pid = dev->idProduct;
        // An AudioControl interface anywhere in the configuration.
        e.audio = false;
        for (uint32_t at = 0; at + 7 <= len && descriptors[at] >= 2; at += descriptors[at]){
            const uint8_t* d = &descriptors[at];
            if (d[1] == 4 && d[0] >= 9 && d[5] == 1 && d[6] == 1) e.audio = true;
        }
        e.product[0] = '\0';
        if (dev->strbuf != nullptr){
            const uint8_t* s = &dev->strbuf->buffer[dev->strbuf->iStrings[strbuf_t::STR_ID_PROD]];
            uint8_t i = 0;
            for (; i < PRODUCT_MAX && s[i] != 0; i++) e.product[i] = (char)s[i];
            e.product[i] = '\0';
        }
        count = count + 1;
        return false;
    }
    void disconnect() override {}

private:
    Entry entries[ENTRIES];
    volatile uint32_t count;
};
#pragma GCC diagnostic pop

EnumerationLog seen_devices;

}  // namespace

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

// One driver's line: its device, or a dash when it holds none.
static void describe_driver(IConsoleIo& out, const char* label, USBDriver& d, const char* note){
    char text[96];
    if (!d){
        snprintf(text, sizeof text, "  %-8s -\r\n", label);
        out.write(text);
        return;
    }
    const uint8_t* product = d.product();
    snprintf(text, sizeof text, "  %-8s %04X:%04X %s%s\r\n", label, d.idVendor(), d.idProduct(),
             product && *product ? (const char*)product : "(no name)", note);
    out.write(text);
}

namespace {

class UsbHostReport final : public IUsbHost {
public:
    void describe(IConsoleIo& out) override {
        out.write("drivers\r\n");
        describe_driver(out, "hub 1", hub1, "");
        describe_driver(out, "hub 2", hub2, "");
        describe_driver(out, "hub 3", hub3, "");
        char label[12];
        for (uint8_t i = 0; i < N_HOST_MIDI_DEVICES; i++){
            snprintf(label, sizeof label, "midi %u", (unsigned)(i + 1));
            MIDIDevice& m = *host_midi[i];
            describe_driver(out, label, m, !m ? "" :
                            host_cable(m) == mmMIDI_HOST_LAUNCHPAD ? "  -> usb host launchpad cable"
                                                                   : "  -> usb host cable");
        }
        mm_audio_describe(out);

        const uint32_t seen = seen_devices.seen();
        char text[96];
        snprintf(text, sizeof text, "enumerated since boot: %lu, newest last\r\n", (unsigned long)seen);
        out.write(text);
        const uint32_t first = seen > EnumerationLog::ENTRIES ? seen - EnumerationLog::ENTRIES : 0;
        for (uint32_t n = first; n < seen; n++){
            const EnumerationLog::Entry& e = seen_devices.entry(n);
            snprintf(text, sizeof text, "  %04X:%04X %s\r\n", e.vid, e.pid,
                     e.product[0] ? e.product : "(no name)");
            out.write(text);
        }
    }
    // What every driver holds, then what was seen since boot and is not
    // among them: a device no driver holds is never told it has gone, so
    // only a held one is connected.
    uint8_t list(UsbDeviceEntry* out, uint8_t max) override {
        uint8_t n = 0;
        add_driver(out, n, max, hub1, USB_ROLE_HUB);
        add_driver(out, n, max, hub2, USB_ROLE_HUB);
        add_driver(out, n, max, hub3, USB_ROLE_HUB);
        for (MIDIDevice* m : host_midi){
            add_driver(out, n, max, *m, host_cable(*m) == mmMIDI_HOST_LAUNCHPAD ? USB_ROLE_LAUNCHPAD : USB_ROLE_MIDI);
        }
        UsbDeviceEntry audio[3];
        const uint8_t held = mm_audio_devices(audio, 3);
        for (uint8_t i = 0; i < held; i++) add(out, n, max, audio[i]);

        const uint32_t seen = seen_devices.seen();
        const uint32_t first = seen > EnumerationLog::ENTRIES ? seen - EnumerationLog::ENTRIES : 0;
        for (uint32_t k = seen; k > first; k--){
            const EnumerationLog::Entry& e = seen_devices.entry(k - 1);
            UsbDeviceEntry entry;
            memset(&entry, 0, sizeof entry);
            entry.vid = e.vid;
            entry.pid = e.pid;
            entry.roles = e.audio ? USB_ROLE_HAS_AUDIO : 0;
            usb_name_copy(entry.name, USB_NAME_BYTES, e.product);
            add(out, n, max, entry);
        }
        return n;
    }

private:
    // Merged with an entry for the same device - by ids and name, which is
    // all a list has - so a Scarlett held for MIDI and audio is one line.
    static void add(UsbDeviceEntry* out, uint8_t& n, uint8_t max, const UsbDeviceEntry& e){
        for (uint8_t i = 0; i < n; i++){
            if (out[i].vid == e.vid && out[i].pid == e.pid && strncmp(out[i].name, e.name, USB_NAME_BYTES) == 0){
                out[i].roles |= e.roles;
                return;
            }
        }
        if (n < max) out[n++] = e;
    }
    static void add_driver(UsbDeviceEntry* out, uint8_t& n, uint8_t max, USBDriver& d, uint8_t role){
        if (!d) return;
        UsbDeviceEntry e;
        memset(&e, 0, sizeof e);
        e.vid = d.idVendor();
        e.pid = d.idProduct();
        e.roles = (uint8_t)(role | USB_ROLE_CONNECTED);
        const uint8_t* product = d.product();
        if (product != nullptr) usb_name_copy(e.name, USB_NAME_BYTES, (const char*)product);
        add(out, n, max, e);
    }
};

UsbHostReport usb_host_report;

}  // namespace
#endif

IUsbHost* mm_usb_host(){
    #ifdef MMMC_USB_HOST
    return &usb_host_report;
    #else
    return nullptr;
    #endif
}

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
