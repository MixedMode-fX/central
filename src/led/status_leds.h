#ifndef MMMC_LED_STATUS_LEDS_H
#define MMMC_LED_STATUS_LEDS_H

#include <stdint.h>
#include "config.h"
#include "hal/ileds.h"

// Two LEDs, and they are the module's entire feedback surface (#7).
//
// `src/hardware.h` declares no display and no RGB LEDs; GREEN_LED and
// RED_LED are all there is. That is not much, so the vocabulary is decided
// deliberately here and documented in the README, because it is the only way
// the module explains itself to a human.
//
// **Green - the clock.**
//   Bright flash on the beat, every MASTER_PPQN ticks, so tempo is visible at
//   a glance and a running module is obviously running.
//   Slow dim heartbeat when no clock is running, so "is it alive?" is always
//   answerable, including on a module with nothing patched into it.
//
// **Red - attention.**
//   Solid: no valid patch, running the defaults. The one state a user has to
//   be able to see without a host attached.
//   Brief flash: something was dropped or refused - a MIDI message, a note
//   bus overflow, a rejected SysEx transfer, a rejected parameter write. The
//   console has the counters behind it (#7's console, `errors`).
//
// **Both - identity and boot.**
//   Three quick alternations at startup, so a power cycle is visible.
//   The same pattern on a device inquiry (#11), so a user with two modules
//   knows which one the editor is talking to.
//
// Nothing here blocks. `service()` is called once per main loop with the
// current time, decides what each LED should be, and writes it; there is no
// delay and no busy wait, so an LED cannot cost a node's `process()` a
// microsecond of jitter.
class StatusLeds {
    public:
        // Brightness levels. Dim is visible in a dark rack and clearly not
        // the same thing as the beat flash.
        static constexpr uint8_t BRIGHT = 255;
        static constexpr uint8_t DIM = 24;
        static constexpr uint8_t OFF = 0;

        // How long a beat flash and an error flash are held. Both are
        // wall-clock, never a number of ticks: a flash measured in ticks
        // would stretch and shrink with tempo.
        static constexpr uint32_t BEAT_FLASH_US = 40000;
        static constexpr uint32_t ERROR_FLASH_US = 120000;
        // The heartbeat when no clock is running: on for HEARTBEAT_ON_US
        // every HEARTBEAT_PERIOD_US.
        static constexpr uint32_t HEARTBEAT_PERIOD_US = 2000000;
        static constexpr uint32_t HEARTBEAT_ON_US = 60000;
        // The boot / identify pattern: this many alternations, each this long.
        static constexpr uint8_t IDENTIFY_BLINKS = 6;
        static constexpr uint32_t IDENTIFY_STEP_US = 120000;

        explicit StatusLeds(ILeds& driver);
        StatusLeds(const StatusLeds&) = delete;
        StatusLeds& operator=(const StatusLeds&) = delete;

        // Events. All of these are cheap flag writes; nothing is driven here.
        void beat(uint32_t now_us);             // a downbeat of MASTER_PPQN ticks
        void error(uint32_t now_us);            // something was dropped or refused
        void identify(uint32_t now_us);         // boot, and #11's device inquiry
        // True while the module is running the default patch because the
        // stored one was missing or invalid: red stays solid until a valid
        // patch is loaded.
        void set_running_defaults(bool on){ defaults = on; }
        void set_clock_running(bool on){ clock_running = on; }

        // Once per main loop.
        void service(uint32_t now_us);

        // What was last written, for the tests and the emulator.
        uint8_t level(uint8_t led) const { return led < LED_COUNT ? last[led] : 0; }
        uint32_t errors() const { return error_count; }

    private:
        void write(uint8_t led, uint8_t brightness);

        ILeds& leds;
        uint32_t beat_until_us;
        uint32_t error_until_us;
        uint32_t identify_until_us;
        uint32_t identify_start_us;
        uint32_t error_count;
        uint8_t last[LED_COUNT];
        bool defaults;
        bool clock_running;
};

#endif
