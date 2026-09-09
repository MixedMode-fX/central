#ifndef MMMC_TEST_FAKE_LEDS_H
#define MMMC_TEST_FAKE_LEDS_H

#include <vector>
#include "hal/ileds.h"

// ILeds for the native tests: keeps the last brightness per LED and records
// every change, so a test can assert on a pattern rather than a snapshot.
class FakeLeds : public ILeds {
    public:
        struct Change { uint8_t led; uint8_t brightness; };
        static constexpr size_t RESERVE = 4096;

        FakeLeds() : levels(), changes() {
            changes.reserve(RESERVE);
            levels[LED_GREEN] = 0;
            levels[LED_RED] = 0;
        }

        void set(uint8_t led, uint8_t brightness) override {
            changes.push_back(Change{led, brightness});
            if (led < LED_COUNT) levels[led] = brightness;
        }

        void clear(){ changes.clear(); }
        // How many times this LED went from off to on.
        uint32_t rises(uint8_t led) const {
            uint32_t n = 0;
            uint8_t last = 0;
            for (const Change& c : changes){
                if (c.led != led) continue;
                if (c.brightness != 0 && last == 0) n++;
                last = c.brightness;
            }
            return n;
        }

        uint8_t levels[LED_COUNT];
        std::vector<Change> changes;
};

#endif
