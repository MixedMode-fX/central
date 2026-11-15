#ifndef MMMC_HAL_TEENSY_PSRAM_H
#define MMMC_HAL_TEENSY_PSRAM_H

#include "hal/psram.h"

// Tests the PSRAM the Teensy core's startup found, end to end, and says how
// it went. Run once from setup(): it takes a moment per megabyte, and it
// overwrites everything above the EXTMEM statics, so nothing may be using
// the chip's heap yet.
PsramReport mm_psram_test();

#endif
