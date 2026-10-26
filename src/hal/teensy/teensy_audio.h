#ifndef MMMC_HAL_TEENSY_AUDIO_H
#define MMMC_HAL_TEENSY_AUDIO_H

// A Focusrite Scarlett on the USB host socket, running the Cumulus reverb
// (lib/CumulusReverb, Clouds' reverb) fully wet: its input 1, mono, into
// both sides of the reverb, and the reverb's left and right out of its
// outputs 1 and 2. Nothing else is claimed as an audio interface.
//
// The interface is the clock: the graph runs from the USB interrupt each
// time a block of its frames has arrived (lib/TeensyUSBAudioHost), in the
// Audio Library's software interrupt, and never touches the main loop. The
// host's Task() is already pumped by mm_midi_read().
//
// Call once from setup(), before mm_midi_setup() starts the host, so the
// wanted format is set before any device can enumerate.
void mm_audio_setup();

#endif
