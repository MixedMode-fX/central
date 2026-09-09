#ifndef MMMC_HAL_TEENSY_CLOCK_H
#define MMMC_HAL_TEENSY_CLOCK_H

#include "clock/master_clock.h"

// Binds the master clock (#4) to the Teensy hardware: an IntervalTimer for
// the subtick counter and a pin interrupt on the sync jack.
//
// The ISRs do one thing each - increment the counter, or timestamp an edge.
// No node's tick() runs in interrupt context: the main loop reads the count
// through MasterClock::consume() and MixedModeMaster::pass() does the work.

// Claims the sync pin and starts the timer. Call once from setup().
void mm_clock_setup(MasterClock& clock);
// Reprograms the timer when the tempo or the measured external period
// changed. Call once per main-loop pass; the timer API is never touched from
// an ISR.
void mm_clock_service();

#endif
