#ifndef MMMC_HAL_IREVERB_H
#define MMMC_HAL_IREVERB_H

#include <stdint.h>

// The one seam between the firmware logic and the reverb on the host port's
// audio interface (hal/teensy/teensy_audio.h): what the Launchpad's reverb
// page moves. The controls are 0..127, as a fader stands for them, and live
// only here: they are not in the patch, and power-on brings back Clouds'
// own settings. The amount is not a control: the reverb is always fully wet.
struct IReverb {
    enum Control : uint8_t { CONTROL_TIME = 0, CONTROL_DIFFUSION, CONTROL_TONE, CONTROL_INPUT, CONTROLS };

    virtual ~IReverb() = default;
    virtual uint8_t get(uint8_t control) const = 0;
    virtual void set(uint8_t control, uint8_t value) = 0;
    // Empties the tank: the tail stops.
    virtual void clear() = 0;
    // An interface is streaming through it, so what moves is heard.
    virtual bool running() const = 0;
};

// The reverb where there is none - the native tests, the emulator, a build
// without the host port's audio: it holds the controls and nothing hears
// them, so the page behaves as it would on the module.
class HeldReverb : public IReverb {
    public:
        // The plug-in's defaults (lib/CumulusReverb), as faders.
        HeldReverb() : values{64, 95, 64, 64}, clears(0) {}
        uint8_t get(uint8_t control) const override { return control < CONTROLS ? values[control] : 0; }
        void set(uint8_t control, uint8_t value) override {
            if (control < CONTROLS) values[control] = value > 127 ? 127 : value;
        }
        void clear() override { clears++; }
        bool running() const override { return false; }
        // How many times the tank was emptied, for the tests.
        uint32_t cleared() const { return clears; }

    private:
        uint8_t values[CONTROLS];
        uint32_t clears;
};

#endif
