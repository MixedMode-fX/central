#ifndef MMMC_HAL_ICONSOLE_IO_H
#define MMMC_HAL_ICONSOLE_IO_H

#include <stdint.h>

// The seam between the console and whatever carries it (#7): USB serial and
// `Serial6` on the module, a string buffer in the tests, a textarea in the
// emulator.
//
// Deliberately byte-at-a-time and non-blocking. The console is polled from
// the main loop between passes, so a host that stops reading must not be able
// to stall the signal path: `write` on a full transmit buffer drops rather
// than waits, and `read` returns false when there is nothing there.
struct IConsoleIo {
    virtual ~IConsoleIo() = default;
    // False when no byte is waiting.
    virtual bool read(uint8_t& out) = 0;
    // Never blocks. A byte that does not fit is dropped: a console is a
    // diagnostic, and losing a line of text is always better than adding
    // jitter to a gate.
    virtual void write(const char* text) = 0;
};

#endif
