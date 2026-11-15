#ifndef MMMC_HAL_PSRAM_H
#define MMMC_HAL_PSRAM_H

#include <stdint.h>

// The PSRAM on the Teensy 4.1's underside pads, as the boot test found it.
// The values are the protocol's (SYSEX_IDENTITY), so they fit a data byte.
enum PsramState : uint8_t {
    PSRAM_ABSENT = 0,   // no chip answered its id
    PSRAM_OK     = 1,   // fitted, and every word held what was written
    PSRAM_FAULT  = 2,   // fitted, and `fault_at` did not
};

struct PsramReport {
    PsramState state;
    uint8_t megabytes;      // what the Teensy core's startup sized, 0 when absent
    uint32_t fault_at;      // byte offset from the start of the chip, when PSRAM_FAULT
};

#endif
