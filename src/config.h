#ifndef MMMC_CONFIG_H
#define MMMC_CONFIG_H

// Framework-free sizing constants shared by the firmware and the native tests.
// Anything that names a physical pin or a transport lives in hardware.h, which
// is only ever included by the Teensy hardware layer (src/hal/teensy/).

#include <stdint.h>

// Number of front-panel I/O ports.
#define GPIO_N 8
// Bitmask selecting every port.
#define ALL_GPIO_MAP ((uint16_t)((1u << GPIO_N) - 1u))

#endif
