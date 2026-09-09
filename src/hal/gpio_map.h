#ifndef MMMC_HAL_GPIO_MAP_H
#define MMMC_HAL_GPIO_MAP_H

#include <stdint.h>
#include "config.h"
#include "hal/igpio.h"

// Bitmask helpers over IGpio. Bit n of a map addresses port n.

inline void gpio_map_mode(IGpio& gpio, uint16_t port_map, uint8_t mode){
    for (uint8_t port = 0; port < GPIO_N; port++){
        const uint16_t mask = (uint16_t)(1u << port);
        if (mask > port_map) break;
        if ((port_map & mask) == mask) gpio.mode(port, mode);
    }
}

inline void gpio_map_write(IGpio& gpio, uint16_t port_map, uint8_t state){
    for (uint8_t port = 0; port < GPIO_N; port++){
        const uint16_t mask = (uint16_t)(1u << port);
        if (mask > port_map) break;
        if ((port_map & mask) == mask) gpio.write(port, state);
    }
}

// Reads every port in the map into result[port]; ports outside the map are
// left untouched.
inline void gpio_map_read(IGpio& gpio, uint16_t port_map, uint8_t *result){
    for (uint8_t port = 0; port < GPIO_N; port++){
        const uint16_t mask = (uint16_t)(1u << port);
        if (mask > port_map) break;
        if ((port_map & mask) == mask) result[port] = gpio.read(port);
    }
}

#endif
