#ifndef MMMC_HAL_IAUDIO_HOST_H
#define MMMC_HAL_IAUDIO_HOST_H

#include <stdint.h>
#include <string.h>
#include "config.h"

// The one seam between the audio rack (audio/rack.h) and the interface on
// the USB host socket (hal/teensy/teensy_audio.h). The rack owns what the
// slots are and says it here; the host builds it out of Audio Library
// objects and reports what the interface is doing. Values are the rack's
// 0..127, as a fader stands for them.

// An interface, as the rack remembers which one to use: by vendor and
// product id, with its name kept so a device that is not plugged in can
// still be named. A zero vendor is any interface.
struct AudioDeviceId {
    uint16_t vid;
    uint16_t pid;
    char name[RACK_DEVICE_NAME_BYTES];

    bool any() const { return vid == 0; }
    bool same(uint16_t v, uint16_t p) const { return vid == v && pid == p; }
};

enum AudioState : uint8_t {
    AUDIO_IDLE = 0,          // no interface
    AUDIO_CONFIGURING,       // control requests in flight
    AUDIO_STREAMING,
    AUDIO_FAILED,            // see AudioStatus::failure
};

// What the interface is doing. `held` is the device the driver has, which
// is not the one chosen until it has been plugged in again: a USB driver
// claims a device when it enumerates and not afterwards.
struct AudioStatus {
    uint8_t state;           // AudioState
    uint32_t rate;           // Hz, while streaming
    uint8_t in_channels, out_channels;
    AudioDeviceId held;      // vid 0 when nothing is held
    const char* failure;     // why, when failed; never null
};

// One slot as the host needs it (audio/rack.h, RackSlot).
struct RackSlot {
    uint8_t plugin;              // RackPlugin
    uint8_t source[2];           // RackSource, left and right
    uint8_t out[2];              // 0, or the interface's output 1..RACK_CHANNELS
    uint8_t params[RACK_PARAMS]; // the plugin's, then the low cut and the level
};

struct IAudioHost {
    virtual ~IAudioHost() = default;
    // The slot whole: its plugin, where it reads and writes, and every
    // parameter. Called at boot and whenever the routing or plugin changes.
    virtual void configure(uint8_t slot, const RackSlot& s) = 0;
    // One parameter, as a fader moves it.
    virtual void set_param(uint8_t slot, uint8_t param, uint8_t value) = 0;
    // Empties the slot's plugin: a tail stops, a tape is erased.
    virtual void clear(uint8_t slot) = 0;
    // The interface to claim the next time one is plugged in.
    virtual void select(const AudioDeviceId& device) = 0;
    virtual void status(AudioStatus& out) const = 0;
};

// The host where there is no interface - the native tests, the emulator, the
// plugin, a build without the host port's audio: it keeps what it was told,
// and nothing hears it.
class HeldAudio : public IAudioHost {
    public:
        HeldAudio() : slots(), chosen(), configures(0), clears(0), last_clear(0xFF) {}
        void configure(uint8_t slot, const RackSlot& s) override {
            if (slot < N_RACK_SLOT) slots[slot] = s;
            configures++;
        }
        void set_param(uint8_t slot, uint8_t param, uint8_t value) override {
            if (slot < N_RACK_SLOT && param < RACK_PARAMS) slots[slot].params[param] = value;
        }
        void clear(uint8_t slot) override { clears++; last_clear = slot; }
        void select(const AudioDeviceId& device) override { chosen = device; }
        void status(AudioStatus& out) const override {
            memset(&out, 0, sizeof out);
            out.state = AUDIO_IDLE;
            out.failure = "";
        }

        // What it was told, for the tests.
        const RackSlot& slot(uint8_t s) const { return slots[s < N_RACK_SLOT ? s : 0]; }
        const AudioDeviceId& selected() const { return chosen; }
        uint32_t configured() const { return configures; }
        uint32_t cleared() const { return clears; }
        uint8_t last_cleared() const { return last_clear; }

    private:
        RackSlot slots[N_RACK_SLOT];
        AudioDeviceId chosen;
        uint32_t configures;
        uint32_t clears;
        uint8_t last_clear;
};

#endif
