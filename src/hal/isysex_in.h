#ifndef MMMC_HAL_ISYSEX_IN_H
#define MMMC_HAL_ISYSEX_IN_H

#include <stdint.h>

// Where a complete incoming SysEx message goes (#11).
//
// SysEx does not travel on a note bus and never becomes a MidiEvent: it is
// control-plane traffic, and the whole point of the protocol is that it works
// whatever the patch does. The transport hands each complete message here,
// tagged with the port it arrived on so a reply can go back the same way.
struct ISysexIn {
    virtual ~ISysexIn() = default;
    // `data` includes the leading F0 and the trailing F7. The buffer belongs
    // to the transport and is only valid for the duration of the call.
    //
    // `now_us` is the same monotonic microsecond clock every other control
    // path receives. Everything a command starts - a transfer's timeout, a
    // learn's timeout, the autosave debounce, the identify blink - is measured
    // from it, so a transport that passed 0 here would have every one of those
    // measured from boot instead.
    virtual void deliver_sysex(uint8_t source, const uint8_t* data, uint16_t length, uint32_t now_us) = 0;
};

#endif
