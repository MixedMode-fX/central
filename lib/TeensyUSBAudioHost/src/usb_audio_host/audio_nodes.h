#ifndef USB_AUDIO_HOST_AUDIO_NODES_H
#define USB_AUDIO_HOST_AUDIO_NODES_H

#include <Arduino.h>
#include <AudioStream.h>
#include "usb_audio_host/config.h"
#include "usb_audio_host/usb_audio_device.h"

// The interface on the host port as Teensy Audio Library objects.
//
//     USBAudioDevice interface(usb);
//     AudioInputUSBHost  in(interface, 2);   // outputs 0..1: the interface's inputs
//     AudioOutputUSBHost out(interface, 2);  // inputs 0..1: the interface's outputs
//
// Call begin() on each after usb.begin(). The input object paces the whole
// graph: every AUDIO_BLOCK_SAMPLES frames the interface delivers, it runs an
// update, so the graph follows the interface's clock and nothing is
// resampled. (A sketch with an I2S or other hardware output as well has two
// clocks; whichever claimed the update first drives, and the other drifts.)
// Channels beyond what the interface streams are silent on the way in and
// dropped on the way out. Samples are the top 16 bits of the interface's.
class AudioInputUSBHost : public AudioStream {
public:
    AudioInputUSBHost(USBAudioDevice& device, uint8_t channels = 2);
    void begin();
    virtual void update(void);

    uint8_t channels() const { return channels_; }
    // Blocks that went out short of frames (the interface stalled), and
    // frames dropped to keep latency bounded (the graph stalled).
    uint32_t underruns() const { return underruns_; }
    uint32_t dropped() const { return dropped_; }
    bool drivesUpdates() const { return update_responsibility_; }

private:
    static void onBlock(void* context);

    USBAudioDevice& device_;
    uint8_t channels_;
    bool update_responsibility_ = false;
    uint32_t underruns_ = 0;
    uint32_t dropped_ = 0;
};

class AudioOutputUSBHost : public AudioStream {
public:
    AudioOutputUSBHost(USBAudioDevice& device, uint8_t channels = 2);
    void begin();
    virtual void update(void);

    uint8_t channels() const { return channels_; }
    bool drivesUpdates() const { return update_responsibility_; }

private:
    static void onBlock(void* context);

    USBAudioDevice& device_;
    uint8_t channels_;
    bool update_responsibility_ = false;
    audio_block_t* queue_[USB_AUDIO_HOST_MAX_CHANNELS] = {};
};

#endif
