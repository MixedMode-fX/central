#ifndef MMMC_HAL_IUSB_HOST_H
#define MMMC_HAL_IUSB_HOST_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "hal/iconsole_io.h"

// What a device on the USB host socket is to the module, as bits. The
// values are the protocol's (SYSEX_USB_DEVICES), so they fit a data byte.
enum UsbRole : uint8_t {
    USB_ROLE_HUB        = 0x01,
    USB_ROLE_MIDI       = 0x02,   // a MIDI driver holds it: the usb host cable
    USB_ROLE_LAUNCHPAD  = 0x04,   // the same, on the surface's cable
    USB_ROLE_AUDIO      = 0x08,   // the rack's driver holds it
    USB_ROLE_AUDIO_IDLE = 0x10,   // an interface the rack is not using
    USB_ROLE_HAS_AUDIO  = 0x20,   // it offered an audio function
    USB_ROLE_CONNECTED  = 0x40,   // plugged in now; otherwise only seen since boot
};

// A device's name into a fixed-width field: as much as fits, the rest zero.
// The field is not terminated when the name fills it.
inline void usb_name_copy(char* out, size_t size, const char* in){
    size_t i = 0;
    for (; in != nullptr && i < size && in[i] != '\0'; i++) out[i] = in[i];
    for (; i < size; i++) out[i] = '\0';
}

struct UsbDeviceEntry {
    uint16_t vid;
    uint16_t pid;
    uint8_t roles;               // UsbRole
    char name[USB_NAME_BYTES];
};

// What is on the USB host socket: what each of the socket's drivers holds,
// and every device the socket has enumerated since boot, claimed or not.
// A device no driver holds is never told it has gone, so only a held one
// can be said to be connected.
struct IUsbHost {
    virtual ~IUsbHost() = default;
    // As text, for the console's `usb`.
    virtual void describe(IConsoleIo& out) = 0;
    // As records, for the protocol: the connected ones first. Returns how
    // many were written, at most `max`.
    virtual uint8_t list(UsbDeviceEntry* out, uint8_t max) = 0;
};

#endif
