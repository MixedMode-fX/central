#ifndef MMMC_TEST_FAKE_EEPROM_H
#define MMMC_TEST_FAKE_EEPROM_H

#include <stdint.h>
#include <stddef.h>
#include "hal/ieeprom.h"
#include "patch/patch_store.h"

// IEeprom for the native tests. Starts at 0xFF everywhere, the way a freshly
// flashed Teensy's emulated EEPROM reads, and counts the writes that actually
// changed a byte so a test can assert that re-saving an unchanged patch costs
// no endurance.
class FakeEeprom : public IEeprom {
    public:
        FakeEeprom() : cells(), changed(0) {
            for (size_t i = 0; i < sizeof cells; i++) cells[i] = 0xFF;
        }

        size_t size() const override { return sizeof cells; }
        uint8_t read(size_t address) const override {
            return address < sizeof cells ? cells[address] : 0xFF;
        }
        void write(size_t address, uint8_t value) override {
            if (address >= sizeof cells) return;
            if (cells[address] == value) return;      // as EEPROM.update() does
            cells[address] = value;
            changed++;
        }

        // Test-side: flip a byte to simulate a corrupted cell.
        void poke(size_t address, uint8_t value){ if (address < sizeof cells) cells[address] = value; }

        uint8_t cells[EEPROM_BYTES];
        uint32_t changed;
};

#endif
