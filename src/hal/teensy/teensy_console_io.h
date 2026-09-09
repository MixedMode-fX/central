#ifndef MMMC_HAL_TEENSY_CONSOLE_IO_H
#define MMMC_HAL_TEENSY_CONSOLE_IO_H

#include "hal/iconsole_io.h"

// IConsoleIo over both consoles at once (#7): USB serial and SERIAL_UART
// (Serial6 at SERIAL_BAUD_RATE). A byte typed on either is read; output goes
// to both, so a session on the UART sees the same thing as one over USB.
//
// Neither is allowed to block. USB serial with no host attached discards
// silently on the Teensy, and the UART write is skipped when its transmit
// buffer is full - a console must never be able to add jitter to a gate.
class TeensyConsoleIo : public IConsoleIo {
    public:
        // Brings up SERIAL_UART. USB serial is already up from main().
        void begin();
        bool read(uint8_t& out) override;
        void write(const char* text) override;
};

#endif
