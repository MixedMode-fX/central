#include "hal/teensy/teensy_includes.h"
#include "hal/teensy/teensy_eeprom.h"
#include "patch/patch_store.h"

#include <EEPROM.h>

// The library reports the emulated size; EEPROM_BYTES is what the firmware
// budgets against, and the two must agree or the slot arithmetic is wrong.
size_t TeensyEeprom::size() const { return (size_t)EEPROM.length(); }

uint8_t TeensyEeprom::read(size_t address) const {
    return address < (size_t)EEPROM.length() ? EEPROM.read((int)address) : 0xFF;
}

void TeensyEeprom::write(size_t address, uint8_t value){
    if (address >= (size_t)EEPROM.length()) return;
    EEPROM.update((int)address, value);       // no write when the byte is unchanged
}
