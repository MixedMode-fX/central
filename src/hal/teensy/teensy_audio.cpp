#include "hal/teensy/teensy_audio.h"

#ifdef MMMC_USB_AUDIO

#include <stdio.h>
#include <string.h>
#include "hal/teensy/teensy_includes.h"
#include "hal/teensy/teensy_midi.h"
#include "config.h"

#ifndef MMMC_USB_HOST
#error "MMMC_USB_AUDIO needs MMMC_USB_HOST: the interface is on the host socket"
#endif

namespace {

// Focusrite-Novation's vendor id, which the Launchpads share: the product
// name is what says Scarlett.
const uint16_t FOCUSRITE_VID = 0x1235;

// USBAudioDevice claims the first audio function offered to it; this one is
// only offered Scarletts. The product name is read during enumeration into a
// string buffer from the host's shared pool, which every device on the hubs
// draws from whatever its driver, so this adds to the pool; a device that
// got no buffer is refused, since it cannot be told apart.
//
// USBHost_t36's drivers have virtual functions and no virtual destructor,
// which -Weffc++ refuses in any class derived from one. This is a static
// object, never deleted through a pointer to its base, so the warning is
// waived for this class alone.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
class ScarlettAudio final : public USBAudioDevice {
public:
    explicit ScarlettAudio(USBHost& host) : USBAudioDevice(host), strings_() {
        USBHost::contribute_String_Buffers(strings_, USB_AUDIO_STRING_BUFFERS);
    }

protected:
    bool claim(Device_t* dev, int type, const uint8_t* descriptors, uint32_t len) override {
        if (!is_scarlett(dev)) return false;
        return USBAudioDevice::claim(dev, type, descriptors, len);
    }

private:
    static bool is_scarlett(const Device_t* dev) {
        if (dev->idVendor != FOCUSRITE_VID || dev->strbuf == nullptr) return false;
        const char* product = reinterpret_cast<const char*>(
            &dev->strbuf->buffer[dev->strbuf->iStrings[strbuf_t::STR_ID_PROD]]);
        return strstr(product, "Scarlett") != nullptr;
    }

    strbuf_t strings_[USB_AUDIO_STRING_BUFFERS];
};
#pragma GCC diagnostic pop

ScarlettAudio scarlett(mm_usb);
AudioInputUSBHost scarlett_in(scarlett, 1);
// In RAM2: its 32 KB of delay memory do not fit beside the module in RAM1.
// RAM2 is not zeroed at boot, which is safe here because the constructors
// set every member the reverb reads. The device object above must stay in
// RAM1, which the controller reads its descriptors from uncached.
DMAMEM AudioEffectVST<CumulusReverb> reverb;
AudioOutputUSBHost scarlett_out(scarlett, 2);

// Mono in, as Clouds normals its right input to its left: the reverb sums
// the two into its tank, so the one input feeds both.
AudioConnection in_to_left(scarlett_in, 0, reverb, 0);
AudioConnection in_to_right(scarlett_in, 0, reverb, 1);
AudioConnection left_to_out(reverb, 0, scarlett_out, 0);
AudioConnection right_to_out(reverb, 1, scarlett_out, 1);

// The Launchpad's reverb page, onto the plug-in's 0..1 parameters. Set from
// the main loop; the plug-in reads them at the top of each block.
class CumulusControls final : public IReverb {
public:
    uint8_t get(uint8_t control) const override {
        if (control >= CONTROLS) return 0;
        return (uint8_t)(reverb.getParameter(PARAM[control]) * 127.0f + 0.5f);
    }
    void set(uint8_t control, uint8_t value) override {
        if (control < CONTROLS) reverb.setParameter(PARAM[control], (value > 127 ? 127 : value) / 127.0f);
    }
    void clear() override { reverb.plugin().clear(); }
    bool running() const override { return scarlett.streaming(); }

private:
    static constexpr int PARAM[CONTROLS] = {CumulusReverb::PARAM_TIME, CumulusReverb::PARAM_DIFFUSION,
                                            CumulusReverb::PARAM_TONE, CumulusReverb::PARAM_INPUT};
};

CumulusControls controls;

}  // namespace

IReverb& mm_reverb(){ return controls; }

void mm_audio_describe(IConsoleIo& out){
    static const char* const STATE[] = {"idle", "configuring", "streaming", "failed"};
    const uint8_t s = (uint8_t)scarlett.state();
    char text[128];
    if (!scarlett){
        snprintf(text, sizeof text, "  %-8s - (%s; only a Scarlett is claimed)\r\n", "audio", STATE[s & 3]);
        out.write(text);
        return;
    }
    const uint8_t* product = scarlett.product();
    snprintf(text, sizeof text, "  %-8s %04X:%04X %s, %s%s%s\r\n", "audio", scarlett.idVendor(),
             scarlett.idProduct(), product && *product ? (const char*)product : "(no name)", STATE[s & 3],
             scarlett.state() == USBAudioDevice::State::Failed ? ": " : "",
             scarlett.state() == USBAudioDevice::State::Failed ? scarlett.failure() : "");
    out.write(text);
    if (scarlett.streaming()){
        snprintf(text, sizeof text, "           %lu Hz, %u in / %u out, %u / %u bits, reverb %s\r\n",
                 (unsigned long)scarlett.sampleRate(), scarlett.inputChannels(), scarlett.outputChannels(),
                 scarlett.inputBits(), scarlett.outputBits(), reverb.bypassed() ? "bypassed" : "wet");
        out.write(text);
    }
}

void mm_audio_setup(){
    AudioMemory(USB_AUDIO_MEMORY_BLOCKS);
    scarlett.setSampleRate(USB_AUDIO_SAMPLE_RATE);
    scarlett.setChannels(1, 2);

    // Fully wet: the reverb's own crossfade all the way over, and the host's
    // blend of its output with its input left at the plug-in's output.
    reverb.setParameter(CumulusReverb::PARAM_AMOUNT, 1.0f);
    reverb.mix(1.0f);

    scarlett_in.begin();
    scarlett_out.begin();
}

#else

static HeldReverb held;
IReverb& mm_reverb(){ return held; }
void mm_audio_describe(IConsoleIo& out){ out.write("  audio    - (not in this build)\r\n"); }
void mm_audio_setup(){}

#endif
