#include "hal/teensy/teensy_audio.h"

#ifdef MMMC_USB_AUDIO

#include <stdio.h>
#include <string.h>
#include "hal/teensy/teensy_includes.h"
#include "hal/teensy/teensy_midi.h"
#include "audio/rack.h"
#include "config.h"

#ifndef MMMC_USB_HOST
#error "MMMC_USB_AUDIO needs MMMC_USB_HOST: the interface is on the host socket"
#endif

namespace {

// The product name a driver read during enumeration, into a string buffer
// from the host's shared pool; empty when the device got none.
void product_of(const Device_t* dev, char* out, size_t size){
    out[0] = '\0';
    if (dev == nullptr || dev->strbuf == nullptr) return;
    const uint8_t* s = &dev->strbuf->buffer[dev->strbuf->iStrings[strbuf_t::STR_ID_PROD]];
    size_t i = 0;
    for (; i + 1 < size && s[i] != 0; i++) out[i] = (char)s[i];
    out[i] = '\0';
}

// The rack's interface: the first audio function offered whose vendor and
// product are the ones chosen, or any when none is. A device is claimed when
// it enumerates, so choosing another takes effect the next time one is
// plugged in.
//
// USBHost_t36's drivers have virtual functions and no virtual destructor,
// which -Weffc++ refuses in any class derived from one. These are static
// objects, never deleted through a pointer to their base, so the warning is
// waived for them alone.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
class RackInterface final : public USBAudioDevice {
public:
    explicit RackInterface(USBHost& host) : USBAudioDevice(host), strings_(), vid_(0), pid_(0) {
        USBHost::contribute_String_Buffers(strings_, USB_AUDIO_STRING_BUFFERS);
    }

    void choose(uint16_t vid, uint16_t pid){ vid_ = vid; pid_ = pid; }
    // Whether this driver, free, would take a device.
    bool wants(const Device_t* dev) const {
        return vid_ == 0 || (dev->idVendor == vid_ && dev->idProduct == pid_);
    }
    bool holding() const { return device != nullptr; }

protected:
    bool claim(Device_t* dev, int type, const uint8_t* descriptors, uint32_t len) override {
        if (!wants(dev)) return false;
        return USBAudioDevice::claim(dev, type, descriptors, len);
    }

private:
    strbuf_t strings_[USB_AUDIO_STRING_BUFFERS];
    volatile uint16_t vid_, pid_;
};

RackInterface interface(mm_usb);

// An audio interface the rack is not using, held only so it can be said to
// be plugged in: a driver that never claims is never told a device has gone.
// It takes the AudioControl interface of a function the rack's driver would
// not take - another device than the one chosen, or any while that driver
// is busy - and does nothing with it. Asked after the rack's driver while
// both are free, and checking what that one wants when it is asked first,
// which it is after a disconnect puts it at the head of the host's list.
class IdleInterface final : public USBDriver {
public:
    IdleInterface() : vid_(0), pid_(0), name_() { driver_ready_for_device(this); }

    bool holding() const { return device != nullptr; }
    uint16_t vid() const { return vid_; }
    uint16_t pid() const { return pid_; }
    const char* name() const { return name_; }

protected:
    bool claim(Device_t* dev, int type, const uint8_t* d, uint32_t len) override {
        if (type != 1 || len < 9) return false;
        // A UAC2 function opens with an interface association; its first
        // interface follows.
        if (d[1] == 11 && len >= 17){ d += d[0]; len -= 8; }
        if (d[0] < 9 || d[1] != 4 || d[5] != 1 || d[6] != 1) return false;
        if (!interface.holding() && interface.wants(dev)) return false;
        vid_ = dev->idVendor;
        pid_ = dev->idProduct;
        product_of(dev, name_, sizeof name_);
        return true;
    }
    void disconnect() override {}

private:
    uint16_t vid_, pid_;
    char name_[USB_NAME_BYTES];
};
#pragma GCC diagnostic pop

IdleInterface idle_0;
IdleInterface idle_1;
IdleInterface* const idle[] = {&idle_0, &idle_1};

// The graph. Constructed in update order: what reads a block after what
// writes it, so a slot sees its sources' block of the same pass - except a
// slot reading another slot, which reads it through that slot's tap and so
// one block later, because the plugins are a pool any slot can draw from.
//
// Everything but the interface's own objects is in RAM2: the plugins' delay
// memory does not fit beside the module in RAM1. RAM2 is not zeroed at boot,
// which is safe because every constructor sets what it reads.
AudioInputUSBHost in(interface, RACK_CHANNELS);
DMAMEM AudioFilterBiquad cut[N_RACK_SLOT][2];
DMAMEM AudioEffectVST<CumulusReverb> verbs[N_RACK_SLOT];
DMAMEM AudioEffectVST<FerricTape> tapes[2];
DMAMEM AudioAmplifier tap[N_RACK_SLOT][2];
DMAMEM AudioMixer4 mix[RACK_CHANNELS];
AudioOutputUSBHost out(interface, RACK_CHANNELS);

static_assert(N_RACK_SLOT <= 4, "an output's mixer has four inputs, one per slot");

// A pool's size is the rack's limit on that plugin (audio/rack.cpp).
bool pools_match_rack(){
    return rack::plugin(RACK_PLUGIN_CUMULUS).instances == sizeof verbs / sizeof verbs[0]
        && rack::plugin(RACK_PLUGIN_FERRIC).instances == sizeof tapes / sizeof tapes[0];
}

AudioConnection mix_out[RACK_CHANNELS] = {
    {mix[0], 0, out, 0}, {mix[1], 0, out, 1}, {mix[2], 0, out, 2}, {mix[3], 0, out, 3},
    {mix[4], 0, out, 4}, {mix[5], 0, out, 5}, {mix[6], 0, out, 6}, {mix[7], 0, out, 7},
};
static_assert(RACK_CHANNELS == 8, "a mixer-to-output connection per channel");

// What a slot is wired with, rewired whenever it is configured.
struct SlotWires {
    AudioConnection source[2];   // its source into its low cut
    AudioConnection feed[2];     // the low cut into the plugin, or into the tap for Thru
    AudioConnection back[2];     // the plugin into the tap
    AudioConnection send[2];     // the tap into its output's mixer
};
SlotWires wires[N_RACK_SLOT];

const int BIQUAD_IDENTITY[5] = {1 << 30, 0, 0, 0, 0};

class RackHost final : public IAudioHost {
public:
    RackHost() : slots_(), verb_of_(), tape_of_() {
        for (uint8_t s = 0; s < N_RACK_SLOT; s++){ verb_of_[s] = NONE; tape_of_[s] = NONE; }
    }

    void configure(uint8_t s, const RackSlot& slot) override {
        if (s >= N_RACK_SLOT) return;
        AudioNoInterrupts();
        unwire(s);
        release(s, slot.plugin);
        slots_[s] = slot;
        acquire(s);
        wire(s);
        for (uint8_t p = 0; p < rack::param_count(slot.plugin); p++) apply(s, p);
        AudioInterrupts();
    }

    void set_param(uint8_t s, uint8_t param, uint8_t value) override {
        if (s >= N_RACK_SLOT || param >= RACK_PARAMS) return;
        slots_[s].params[param] = value;
        apply(s, param);
    }

    void clear(uint8_t s) override {
        if (s >= N_RACK_SLOT) return;
        if (verb_of_[s] != NONE) verbs[verb_of_[s]].plugin().clear();
        if (tape_of_[s] != NONE) tapes[tape_of_[s]].plugin().clear();
    }

    void select(const AudioDeviceId& d) override { interface.choose(d.vid, d.vid ? d.pid : 0); }

    void status(AudioStatus& st) const override {
        memset(&st, 0, sizeof st);
        static const uint8_t STATE[] = {AUDIO_IDLE, AUDIO_CONFIGURING, AUDIO_STREAMING, AUDIO_FAILED};
        st.state = STATE[(uint8_t)interface.state() & 3];
        st.failure = interface.state() == USBAudioDevice::State::Failed && interface.failure()
                   ? interface.failure() : "";
        if (interface.streaming()){
            st.rate = interface.sampleRate();
            st.in_channels = interface.inputChannels();
            st.out_channels = interface.outputChannels();
        }
        if (interface){
            st.held.vid = interface.idVendor();
            st.held.pid = interface.idProduct();
            const uint8_t* product = interface.product();
            if (product != nullptr) usb_name_copy(st.held.name, RACK_DEVICE_NAME_BYTES, (const char*)product);
        }
    }

private:
    static constexpr uint8_t NONE = 0xFF;

    AudioStream* plugin_of(uint8_t s){
        if (verb_of_[s] != NONE) return &verbs[verb_of_[s]];
        if (tape_of_[s] != NONE) return &tapes[tape_of_[s]];
        return nullptr;
    }

    // Gives a slot's instance back to its pool unless it keeps its plugin.
    void release(uint8_t s, uint8_t next){
        if (slots_[s].plugin == next) return;
        verb_of_[s] = NONE;
        tape_of_[s] = NONE;
    }

    template <class Pool>
    static uint8_t free_in(Pool& pool, uint8_t n, const uint8_t* taken){
        for (uint8_t i = 0; i < n; i++){
            bool used = false;
            for (uint8_t s = 0; s < N_RACK_SLOT; s++) used = used || taken[s] == i;
            if (!used) return i;
        }
        (void)pool;
        return NONE;
    }

    // A fresh instance from the pool: emptied, fully wet at the host, and
    // given every parameter by configure().
    void acquire(uint8_t s){
        const uint8_t plugin = slots_[s].plugin;
        if (plugin == RACK_PLUGIN_CUMULUS && verb_of_[s] == NONE){
            verb_of_[s] = free_in(verbs, N_RACK_SLOT, verb_of_);
            if (verb_of_[s] != NONE){ verbs[verb_of_[s]].plugin().clear(); verbs[verb_of_[s]].mix(1.0f); }
        }
        if (plugin == RACK_PLUGIN_FERRIC && tape_of_[s] == NONE){
            tape_of_[s] = free_in(tapes, 2, tape_of_);
            if (tape_of_[s] != NONE){ tapes[tape_of_[s]].plugin().clear(); tapes[tape_of_[s]].mix(1.0f); }
        }
    }

    void unwire(uint8_t s){
        SlotWires& w = wires[s];
        for (uint8_t side = 0; side < 2; side++){
            w.source[side].disconnect();
            w.feed[side].disconnect();
            w.back[side].disconnect();
            w.send[side].disconnect();
        }
    }

    void wire(uint8_t s){
        const RackSlot& slot = slots_[s];
        if (slot.plugin == RACK_PLUGIN_NONE) return;
        SlotWires& w = wires[s];
        AudioStream* plugin = plugin_of(s);
        if (slot.plugin != RACK_PLUGIN_THRU && plugin == nullptr) return;
        for (uint8_t side = 0; side < 2; side++){
            const uint8_t src = slot.source[side];
            if (src >= RACK_SOURCE_SLOT){
                const uint8_t from = (uint8_t)((src - RACK_SOURCE_SLOT) / 2u), from_side = (uint8_t)((src - RACK_SOURCE_SLOT) % 2u);
                if (from < N_RACK_SLOT) w.source[side].connect(tap[from][from_side], 0, cut[s][side], 0);
            } else if (src >= RACK_SOURCE_INPUT){
                w.source[side].connect(in, (uint8_t)(src - RACK_SOURCE_INPUT), cut[s][side], 0);
            }
            if (plugin == nullptr){
                w.feed[side].connect(cut[s][side], 0, tap[s][side], 0);
            } else {
                w.feed[side].connect(cut[s][side], 0, *plugin, side);
                w.back[side].connect(*plugin, side, tap[s][side], 0);
            }
            if (slot.out[side] != 0) w.send[side].connect(tap[s][side], 0, mix[slot.out[side] - 1], s);
        }
    }

    void apply(uint8_t s, uint8_t p){
        const RackSlot& slot = slots_[s];
        const uint8_t value = slot.params[p];
        if (p == rack::low_cut_param(slot.plugin)){
            // Two Butterworth stages, 24 dB an octave, as the tape sketch's send.
            const float hz = rack::low_cut_hz(value);
            for (uint8_t side = 0; side < 2; side++){
                for (uint8_t stage = 0; stage < 2; stage++){
                    if (hz > 0.0f) cut[s][side].setHighpass(stage, hz, 0.7071f);
                    else cut[s][side].setCoefficients(stage, BIQUAD_IDENTITY);
                }
            }
            return;
        }
        if (p == rack::level_param(slot.plugin)){
            const float gain = rack::level_gain(value);
            for (uint8_t side = 0; side < 2; side++){
                if (slot.out[side] != 0) mix[slot.out[side] - 1].gain(s, gain);
            }
            return;
        }
        const float v = (float)value / 127.0f;
        if (verb_of_[s] != NONE) verbs[verb_of_[s]].setParameter(p, v);
        if (tape_of_[s] != NONE) tapes[tape_of_[s]].setParameter(p, v);
    }

    RackSlot slots_[N_RACK_SLOT];
    uint8_t verb_of_[N_RACK_SLOT];   // the instance each slot holds, or NONE
    uint8_t tape_of_[N_RACK_SLOT];
};

RackHost host;

const char* state_name(){
    static const char* const STATE[] = {"idle", "configuring", "streaming", "failed"};
    return STATE[(uint8_t)interface.state() & 3];
}

}  // namespace

IAudioHost& mm_audio_host(){ return host; }

uint8_t mm_audio_devices(UsbDeviceEntry* out, uint8_t max){
    uint8_t n = 0;
    if (interface && n < max){
        UsbDeviceEntry& e = out[n++];
        memset(&e, 0, sizeof e);
        e.vid = interface.idVendor();
        e.pid = interface.idProduct();
        e.roles = USB_ROLE_AUDIO | USB_ROLE_HAS_AUDIO | USB_ROLE_CONNECTED;
        const uint8_t* product = interface.product();
        if (product != nullptr) usb_name_copy(e.name, USB_NAME_BYTES, (const char*)product);
    }
    for (IdleInterface* d : idle){
        if (!d->holding() || n >= max) continue;
        UsbDeviceEntry& e = out[n++];
        memset(&e, 0, sizeof e);
        e.vid = d->vid();
        e.pid = d->pid();
        e.roles = USB_ROLE_AUDIO_IDLE | USB_ROLE_HAS_AUDIO | USB_ROLE_CONNECTED;
        usb_name_copy(e.name, USB_NAME_BYTES, d->name());
    }
    return n;
}

void mm_audio_describe(IConsoleIo& out_io){
    char text[128];
    if (!interface){
        snprintf(text, sizeof text, "  %-8s - (%s)\r\n", "audio", state_name());
        out_io.write(text);
    } else {
        const uint8_t* product = interface.product();
        snprintf(text, sizeof text, "  %-8s %04X:%04X %s, %s%s%s\r\n", "audio", interface.idVendor(),
                 interface.idProduct(), product && *product ? (const char*)product : "(no name)", state_name(),
                 interface.state() == USBAudioDevice::State::Failed ? ": " : "",
                 interface.state() == USBAudioDevice::State::Failed ? interface.failure() : "");
        out_io.write(text);
        if (interface.streaming()){
            snprintf(text, sizeof text, "           %lu Hz, %u in / %u out, %u / %u bits\r\n",
                     (unsigned long)interface.sampleRate(), interface.inputChannels(), interface.outputChannels(),
                     interface.inputBits(), interface.outputBits());
            out_io.write(text);
        }
    }
    for (IdleInterface* d : idle){
        if (!d->holding()) continue;
        snprintf(text, sizeof text, "  %-8s %04X:%04X %s, not the rack's\r\n", "audio", d->vid(), d->pid(),
                 d->name()[0] ? d->name() : "(no name)");
        out_io.write(text);
    }
}

void mm_audio_setup(){
    AudioMemory(USB_AUDIO_MEMORY_BLOCKS);
    interface.setSampleRate(USB_AUDIO_SAMPLE_RATE);
    // As many channels as a slot can be routed to: the driver picks the
    // alternate setting closest to it, more rather than fewer.
    interface.setChannels(RACK_CHANNELS, RACK_CHANNELS);
    if (!pools_match_rack()) Serial.println("audio: the plugin pools disagree with the rack's limits");
    for (uint8_t s = 0; s < N_RACK_SLOT; s++){
        for (uint8_t side = 0; side < 2; side++){
            cut[s][side].setCoefficients(0, BIQUAD_IDENTITY);
            tap[s][side].gain(1.0f);
        }
    }
    in.begin();
    out.begin();
}

#else

static HeldAudio held;
IAudioHost& mm_audio_host(){ return held; }
uint8_t mm_audio_devices(UsbDeviceEntry*, uint8_t){ return 0; }
void mm_audio_describe(IConsoleIo& out){ out.write("  audio    - (not in this build)\r\n"); }
void mm_audio_setup(){}

#endif
