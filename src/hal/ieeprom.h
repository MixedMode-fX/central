#ifndef MMMC_HAL_IEEPROM_H
#define MMMC_HAL_IEEPROM_H

#include <stdint.h>
#include <stddef.h>

// The one seam between the firmware logic and non-volatile storage (#7).
//
// On the Teensy 4.1 this is flash-emulated EEPROM: about 4 KB, with limited
// write endurance, and a write costs far more than a read. Nothing on this
// interface is called from an interrupt, and PatchStore never writes on a
// parameter change - only on an explicit save, debounced.
//
// `write` is expected to be a no-op when the byte is already what is being
// written; the Teensy library does that for us, and the fake does it too, so
// re-saving an unchanged patch costs no endurance.
struct IEeprom {
    virtual ~IEeprom() = default;
    virtual size_t size() const = 0;
    virtual uint8_t read(size_t address) const = 0;
    virtual void write(size_t address, uint8_t value) = 0;
};

#endif
