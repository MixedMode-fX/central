#ifndef MMMC_HAL_TEENSY_EEPROM_H
#define MMMC_HAL_TEENSY_EEPROM_H

#include "hal/ieeprom.h"

// IEeprom over the Teensy's flash-emulated EEPROM.
//
// EEPROM.update() is what write() calls: it reads first and only erases and
// rewrites when the byte actually differs, so re-saving an unchanged patch
// costs no endurance at all. That is what lets PatchStore rewrite slot 0 on
// a debounce without worrying about wearing the flash out.
class TeensyEeprom : public IEeprom {
    public:
        size_t size() const override;
        uint8_t read(size_t address) const override;
        void write(size_t address, uint8_t value) override;
};

#endif
