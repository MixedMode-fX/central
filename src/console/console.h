#ifndef MMMC_CONSOLE_CONSOLE_H
#define MMMC_CONSOLE_CONSOLE_H

#include <stdint.h>
#include "config.h"
#include "hal/iconsole_io.h"
#include "patch/patch_manager.h"
#include "control/cc_mapper.h"

// The text console (#7): with no display and no panel controls, this and the
// SysEx protocol (#11) are the only two ways anyone sees inside the module or
// changes anything about it.
//
// It is built early rather than late on purpose. It is the **interim
// configuration path** while #11 is being written - dump, load, set one
// field, list algorithms, all in text - so real testing on hardware is not
// blocked on a binary protocol; and it stays afterwards as the fallback when
// a host cannot speak SysEx, and as the only way to read the error counters
// behind the red LED.
//
// **It works whatever patch is loaded.** The console is not a graph node, it
// is not reachable from a bus, and a patch cannot disable it or reroute its
// transport. With no button to hold at power-on, a module that could be
// talked out of listening would be a module that has to be reflashed to
// recover.
//
// Nothing here blocks or allocates. `service()` reads whatever bytes are
// waiting, runs a command when a line completes, and returns; a line longer
// than CONSOLE_LINE_MAX is truncated rather than growing a buffer.
class Console {
    public:
        static constexpr uint8_t CONSOLE_LINE_MAX = 96;
        // Enough for the widest command: `map <slot> <cc> <node> <param>
        // <min> <max>` is seven words including the verb.
        static constexpr uint8_t MAX_ARGS = 8;

        Console(IConsoleIo& io, PatchManager& patches, MixedModeMaster& master,
                PatchStore& store, StatusLeds& leds, CcMapper& cc);
        Console(const Console&) = delete;
        Console& operator=(const Console&) = delete;

        // Prints the banner. Call once after boot.
        void greet();
        // Once per main loop. `now_us` is passed to whatever the command
        // does, so a command that changes the patch is timestamped like any
        // other edit.
        void service(uint32_t now_us);

        // Feeds one line directly, for the tests and the emulator.
        void execute(const char* line, uint32_t now_us);

    private:
        void prompt();
        void dispatch(uint32_t now_us);

        void cmd_help();
        void cmd_info();
        void cmd_clock(uint8_t argc, uint32_t now_us);
        void cmd_patch();
        void cmd_buses();
        void cmd_errors();
        void cmd_algorithms(uint8_t argc);
        void cmd_params(uint8_t argc);
        void cmd_set(uint8_t argc, uint32_t now_us);
        void cmd_get(uint8_t argc);
        void cmd_slots();
        void cmd_save(uint8_t argc);
        void cmd_load(uint8_t argc, uint32_t now_us);
        void cmd_erase(uint8_t argc);
        void cmd_defaults(uint32_t now_us);
        void cmd_maps();
        void cmd_map(uint8_t argc, uint32_t now_us);
        void cmd_unmap(uint8_t argc, uint32_t now_us);
        void cmd_learn(uint8_t argc, uint32_t now_us);

        void put(const char* text){ io.write(text); }
        void put_line(const char* text){ io.write(text); io.write("\r\n"); }
        void put_uint(uint32_t value);
        void put_int(int32_t value);
        void put_kv(const char* key, uint32_t value);
        // Splits `line` into argv in place. Returns the argument count.
        uint8_t split();
        // Parses argv[index] as an unsigned number. `ok` is false when the
        // argument is missing or not a number, so a typo is reported rather
        // than silently read as zero.
        uint32_t arg_uint(uint8_t index, bool& ok) const;

        IConsoleIo& io;
        PatchManager& patches;
        MixedModeMaster& mm;
        PatchStore& store;
        StatusLeds& leds;
        CcMapper& cc;
        char line[CONSOLE_LINE_MAX + 1];
        char* argv[MAX_ARGS];
        uint8_t argc;
        uint8_t length;
        bool overflowed;      // the line was longer than CONSOLE_LINE_MAX
};

#endif
