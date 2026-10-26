#ifndef USB_AUDIO_HOST_H
#define USB_AUDIO_HOST_H

// USB Audio Class host for the Teensy host port, on top of USBHost_t36.
//
//   USBAudioDevice      the interface: enumeration, sample rate, streams, rings
//   AudioInputUSBHost   its inputs as a Teensy Audio Library source
//   AudioOutputUSBHost  its outputs as a Teensy Audio Library sink
//
// Sizing constants are in usb_audio_host/config.h and can be overridden from
// the build.

#include "usb_audio_host/usb_audio_device.h"
#include "usb_audio_host/audio_nodes.h"

#endif
