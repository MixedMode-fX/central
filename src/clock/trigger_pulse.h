#ifndef MMMC_CLOCK_TRIGGER_PULSE_H
#define MMMC_CLOCK_TRIGGER_PULSE_H

#include <stdint.h>
#include "config.h"

// A fixed-width output pulse (#4).
//
// Trigger width is wall-clock time, never a number of ticks: a 24-PPQN tick
// at 120 BPM is ~20 ms, and a Eurorack trigger is 1-10 ms, so a width
// expressed in ticks would grow and shrink with tempo and stop being a
// trigger. The node fires the pulse when its pattern says so and asks for the
// level every pass; the pulse retriggers cleanly if it is fired again while
// still high, which is what happens when the width is longer than the period.
class TriggerPulse {
    public:
        TriggerPulse() : width_us(TRIGGER_WIDTH_US), start_us(0), active(false) {}

        void set_width_us(uint32_t us){
            if (us < TRIGGER_MIN_WIDTH_US) us = TRIGGER_MIN_WIDTH_US;
            if (us > TRIGGER_MAX_WIDTH_US) us = TRIGGER_MAX_WIDTH_US;
            width_us = us;
        }
        uint32_t width() const { return width_us; }

        void fire(uint32_t now_us){ start_us = now_us; active = true; }
        void clear(){ active = false; }

        // True while the pulse is high. `now_us` may wrap; the subtraction is
        // unsigned, so a wrap between fire() and level() is not a 71-minute
        // pulse.
        bool level(uint32_t now_us){
            if (!active) return false;
            if ((uint32_t)(now_us - start_us) < width_us) return true;
            active = false;
            return false;
        }

    private:
        uint32_t width_us;
        uint32_t start_us;
        bool active;
};

#endif
