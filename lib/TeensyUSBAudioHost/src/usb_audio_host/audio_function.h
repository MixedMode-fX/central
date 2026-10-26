#ifndef USB_AUDIO_HOST_AUDIO_FUNCTION_H
#define USB_AUDIO_HOST_AUDIO_FUNCTION_H

#include <stdint.h>
#include "usb_audio_host/config.h"

// The audio function of a USB device as its descriptors describe it: one
// AudioControl interface, the clock and terminal entities behind it, and every
// alternate setting of its AudioStreaming interfaces that carries data.
//
// Framework-free: the parser reads bytes and fills plain structs, so it is
// unit-tested on the host (test/test_audio_function) against descriptor
// images of real interfaces.
namespace usb_audio_host {

enum class UacVersion : uint8_t { None = 0, V1 = 1, V2 = 2 };

enum class SyncType : uint8_t { None = 0, Async = 1, Adaptive = 2, Sync = 3 };

struct Endpoint {
    uint8_t address = 0;    // bEndpointAddress; bit 7 set = IN
    uint8_t attributes = 0; // bmAttributes
    uint16_t max_packet = 0; // bytes per transaction (wMaxPacketSize bits 10:0)
    uint8_t mult = 1;        // transactions per microframe (wMaxPacketSize bits 12:11, plus one)
    uint8_t interval = 1;    // bInterval

    bool valid() const { return address != 0; }
    bool in() const { return (address & 0x80) != 0; }
    uint8_t number() const { return address & 0x0F; }
    SyncType sync() const { return static_cast<SyncType>((attributes >> 2) & 0x03); }
    bool isFeedback() const { return (attributes & 0x30) == 0x10; }
    bool isImplicitFeedback() const { return (attributes & 0x30) == 0x20; }
};

// One alternate setting of an AudioStreaming interface with a data endpoint.
struct Format {
    uint8_t interface = 0;
    uint8_t alt = 0;
    uint8_t terminal_link = 0;
    uint8_t channels = 0;
    uint8_t subslot = 0; // bytes per sample on the wire
    uint8_t bits = 0;    // bBitResolution
    bool pcm = false;
    bool rate_control = false; // UAC1: the endpoint takes SET_CUR sampling frequency
    Endpoint data = {};
    Endpoint feedback = {}; // explicit feedback endpoint of an asynchronous sink, if any
    // UAC1 sample rates from the format descriptor. n_rates == 0 with
    // rate_max > 0 is a continuous range. UAC2 formats carry none: their
    // rates belong to the clock source and are queried at run time.
    uint8_t n_rates = 0;
    uint32_t rates[USB_AUDIO_HOST_MAX_RATES] = {};
    uint32_t rate_min = 0;
    uint32_t rate_max = 0;

    bool input() const { return data.in(); }
    // rate 0 asks whether the format has any rate at all.
    bool supportsRate(uint32_t rate) const;
    uint16_t packetBytes() const { return static_cast<uint16_t>(data.max_packet * data.mult); }
};

struct Terminal {
    uint8_t id = 0;
    uint16_t type = 0;
    bool input = false;
    uint8_t clock_id = 0; // UAC2 bCSourceID
    uint8_t channels = 0; // input terminals
};

struct ClockEntity {
    uint8_t id = 0;
    uint8_t subtype = 0;   // AC_CLOCK_SOURCE, AC_CLOCK_SELECTOR or AC_CLOCK_MULTIPLIER
    uint8_t source_id = 0; // selector: its first pin; multiplier: bCSourceID
    uint8_t n_pins = 0;    // selector
    uint8_t attributes = 0;
    uint8_t controls = 0;

    bool programmable() const;
};

// The clock entities a streaming format's samples are timed by, resolved
// through selectors and multipliers to the clock source that takes the
// sampling frequency request.
struct ClockPath {
    uint8_t source = 0;       // clock source entity, 0 when the function has none (UAC1)
    uint8_t selector = 0;     // clock selector on the way, 0 when none
    uint8_t selector_pin = 0; // 1-based pin of the selector that reaches `source`
};

class AudioFunction {
public:
    AudioFunction() = default;

    // Parses from an AudioControl interface descriptor to the end of the
    // configuration, or to the first interface that is not part of this
    // function. Returns false when the slice is not an AudioControl interface.
    bool parse(const uint8_t* descriptors, uint32_t length);
    void clear();

    UacVersion version() const { return version_; }
    uint8_t controlInterface() const { return control_interface_; }
    uint8_t formatCount() const { return n_formats_; }
    const Format& format(uint8_t i) const { return formats_[i]; }
    uint8_t terminalCount() const { return n_terminals_; }
    const Terminal& terminal(uint8_t i) const { return terminals_[i]; }
    uint8_t clockCount() const { return n_clocks_; }
    const ClockEntity& clock(uint8_t i) const { return clocks_[i]; }

    // The format to stream in one direction: PCM, at least 16 bits, packets
    // that fit the engine's buffers, the channel count closest to `channels`
    // (preferring more over fewer), then the highest resolution. UAC1
    // formats must list `rate` (0: any rate). nullptr when the function has
    // no usable format in that direction.
    const Format* choose(bool input, uint8_t channels, uint32_t rate, uint16_t packet_limit) const;

    // The IN format that feeds back the rate of an asynchronous OUT format
    // without a feedback endpoint: the generic UAC2 rule (an IN data endpoint
    // marked implicit feedback) and Linux's broader one (any asynchronous
    // IN endpoint of the function). nullptr when the sink is not
    // asynchronous or has its own feedback endpoint.
    const Format* implicitFeedbackSource(const Format& sink) const;

    bool clockFor(const Format& f, ClockPath& path) const;
    const Terminal* findTerminal(uint8_t id) const;
    const ClockEntity* findClock(uint8_t id) const;

private:
    void parseControl(const uint8_t* d);
    void parseStreaming(const uint8_t* d, Format& pending);
    bool commit(Format& pending);

    UacVersion version_ = UacVersion::None;
    uint8_t control_interface_ = 0;
    uint8_t n_formats_ = 0;
    uint8_t n_terminals_ = 0;
    uint8_t n_clocks_ = 0;
    Format formats_[USB_AUDIO_HOST_MAX_FORMATS] = {};
    Terminal terminals_[USB_AUDIO_HOST_MAX_ENTITIES] = {};
    ClockEntity clocks_[USB_AUDIO_HOST_MAX_ENTITIES] = {};
};

} // namespace usb_audio_host

#endif
