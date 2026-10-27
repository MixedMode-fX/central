#ifndef MMMC_HAL_IUSB_HOST_H
#define MMMC_HAL_IUSB_HOST_H

#include "hal/iconsole_io.h"

// What is on the USB host socket, for the console's `usb` command: what
// each of the socket's drivers holds, and every device the socket has
// enumerated since boot, claimed or not. Text, because a person reads it and
// only the hardware layer knows its drivers.
struct IUsbHost {
    virtual ~IUsbHost() = default;
    virtual void describe(IConsoleIo& out) = 0;
};

#endif
