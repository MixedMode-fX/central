#ifndef MMMC_HAL_IMIDI_OUT_H
#define MMMC_HAL_IMIDI_OUT_H

#include <stdint.h>
#include "hal/midi_types.h"

// The one seam between the firmware logic and the MIDI transports.
// `target` is a MidiPort bitmask; the message goes to every set bit.
struct IMidiOut {
    virtual ~IMidiOut() = default;
    virtual void send(uint8_t target, uint8_t type,
                      uint8_t d1, uint8_t d2, uint8_t channel) = 0;
    // One complete SysEx message including its leading F0 and trailing F7
    // (#11). A reply goes back to the port the request arrived on, so a
    // module answers where it was asked rather than shouting on every wire.
    virtual void send_sysex(uint8_t target, const uint8_t* data, uint16_t length) = 0;
};

#endif
