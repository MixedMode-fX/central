#ifndef MMMC_HAL_TEENSY_AUDIO_H
#define MMMC_HAL_TEENSY_AUDIO_H

#include "hal/iaudio_host.h"
#include "hal/iconsole_io.h"
#include "hal/iusb_host.h"

// The audio interface on the USB host socket, running the audio rack
// (audio/rack.h): every channel of the interface in, a low cut and a plugin
// per slot from a pool of them (lib/CumulusReverb, lib/FerricTape), and a
// mixer per output summing the slots that write it.
//
// The interface is the clock: the graph runs from the USB interrupt each
// time a block of its frames has arrived (TeensyUSBAudioHost), in the Audio
// Library's software interrupt, and never touches the main loop. The host's
// Task() is already pumped by mm_midi_read().
//
// Call once from setup(), before mm_midi_setup() starts the host, so the
// wanted format is set before any device can enumerate.
void mm_audio_setup();

// What the rack drives. In a build without MMMC_USB_AUDIO, a HeldAudio
// nothing hears.
IAudioHost& mm_audio_host();

// The audio interface's lines in the console's `usb`: what the drivers
// hold, how far the rack's got, and what it negotiated.
void mm_audio_describe(IConsoleIo& out);

// The interfaces the audio drivers hold, for the USB list: the rack's, then
// those it is not using. Returns how many were written.
uint8_t mm_audio_devices(UsbDeviceEntry* out, uint8_t max);

#endif
