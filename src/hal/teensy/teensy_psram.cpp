#include "hal/teensy/teensy_includes.h"
#include "hal/teensy/teensy_psram.h"
#include "memory/memtest.h"

// Set by the core's startup (cores/teensy4/startup.c) from the chips' ids:
// megabytes, 0 when neither pad answered.
extern "C" uint8_t external_psram_size;
// Where the linker put the end of the EXTMEM statics. The test starts above them, so it
// never clobbers one a constructor has already set.
extern "C" unsigned long _extram_end;

static constexpr uint32_t PSRAM_BASE = 0x70000000u;

static void settle(volatile uint32_t* base, size_t words){
    arm_dcache_flush_delete((void*)base, (uint32_t)(words * sizeof(uint32_t)));
}

PsramReport mm_psram_test(){
    PsramReport report{PSRAM_ABSENT, external_psram_size, 0};
    if (external_psram_size == 0) return report;

    const uint32_t start = ((uint32_t)&_extram_end + 31u) & ~31u;
    const uint32_t end = PSRAM_BASE + (uint32_t)external_psram_size * 0x100000u;
    if (start >= end){ report.state = PSRAM_OK; return report; }

    const size_t words = (end - start) / sizeof(uint32_t);
    const size_t bad = memtest::run((volatile uint32_t*)start, words, settle);
    if (bad == words){
        report.state = PSRAM_OK;
    } else {
        report.state = PSRAM_FAULT;
        report.fault_at = start - PSRAM_BASE + (uint32_t)(bad * sizeof(uint32_t));
    }
    return report;
}
