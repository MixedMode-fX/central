#ifndef MMMC_HAL_TEENSY_GPIO_H
#define MMMC_HAL_TEENSY_GPIO_H

#include "hal/igpio.h"
#include "config.h"

// IGpio over the Teensy digital pins. Owns a copy of the port -> pin table.
class TeensyGpio : public IGpio {
    public:
        explicit TeensyGpio(const uint8_t (&pins)[GPIO_N]);

        void mode(uint8_t port, uint8_t mode) override;
        void write(uint8_t port, uint8_t state) override;
        uint8_t read(uint8_t port) override;

    private:
        uint8_t pins_[GPIO_N];
};

#endif
