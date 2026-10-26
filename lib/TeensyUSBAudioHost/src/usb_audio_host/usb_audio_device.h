#ifndef USB_AUDIO_HOST_USB_AUDIO_DEVICE_H
#define USB_AUDIO_HOST_USB_AUDIO_DEVICE_H

#include <stdint.h>
#include <Arduino.h>
#include <USBHost_t36.h>
#include "usb_audio_host/config.h"
#include "usb_audio_host/audio_function.h"
#include "usb_audio_host/iso_engine.h"
#include "usb_audio_host/pacer.h"
#include "usb_audio_host/sample_ring.h"

// A class-compliant USB audio interface on the host port, as a USBHost_t36
// driver. Create one per interface you want to serve, alongside the host's
// other drivers:
//
//     USBHost usb;
//     USBHub hub(usb);
//     USBAudioDevice audio(usb);
//
// It claims the AudioControl interface of the first UAC1 or UAC2 function
// offered to it, reads the function's descriptors, sets the sample rate,
// selects one alternate setting per direction and streams both: the input
// into a ring the sketch reads, the output from a ring the sketch writes.
// Samples are int32_t, left-justified, interleaved by channel. The Audio
// Library objects in audio_nodes.h are the usual readers and writers.
//
// Everything after claim() happens inside the USB interrupt: the control
// requests, the packet callbacks, the rings' far ends. The public API is
// safe to call from the sketch at any time.
class USBAudioDevice : public USBDriver {
public:
    explicit USBAudioDevice(USBHost& host);

    // What to ask of the next device. Effective at the next connection.
    void setSampleRate(uint32_t hz) { wanted_rate_ = hz; }
    void setChannels(uint8_t input, uint8_t output) { wanted_in_ = input; wanted_out_ = output; }

    enum class State : uint8_t {
        Idle,         // no device
        Configuring,  // control requests in flight
        Streaming,    // isochronous streams running
        Failed        // see failure()
    };
    State state() const { return state_; }
    bool streaming() const { return state_ == State::Streaming; }
    const char* failure() const { return failure_; }

    // The streams as negotiated. Zero until streaming.
    uint32_t sampleRate() const { return rate_; }
    uint8_t inputChannels() const { return in_format_ ? in_format_->channels : 0; }
    uint8_t outputChannels() const { return out_format_ ? out_format_->channels : 0; }
    uint8_t inputBits() const { return in_format_ ? in_format_->bits : 0; }
    uint8_t outputBits() const { return out_format_ ? out_format_->bits : 0; }
    bool highSpeed() const { return high_speed_; }
    usb_audio_host::UacVersion version() const { return function_.version(); }
    const usb_audio_host::AudioFunction& function() const { return function_; }

    // Input: frames the interface has delivered, interleaved by
    // inputChannels(). read() zero-fills what is not there.
    uint32_t inputAvailable() const { return in_ring_.available(); }
    uint32_t readInput(int32_t* frames, uint32_t count) { return in_ring_.read(frames, count); }
    uint32_t discardInput(uint32_t count) { return in_ring_.discard(count); }

    // Output: frames for the interface, interleaved by outputChannels().
    uint32_t outputSpace() const { return out_ring_.space(); }
    uint32_t outputAvailable() const { return out_ring_.available(); }
    uint32_t writeOutput(const int32_t* frames, uint32_t count) { return out_ring_.write(frames, count); }

    // Block clock. `fn` is called from the USB interrupt whenever at least
    // `frames` input frames are waiting and no earlier call is still
    // unanswered; blockTaken() answers it. A device with no input stream
    // calls when the output ring wants a block instead. This is how the
    // Audio Library objects pace the graph from the interface's clock.
    typedef void (*BlockFn)(void* context);
    void onBlock(BlockFn fn, void* context, uint16_t frames);
    void blockTaken() { block_pending_ = false; }
    bool hasBlockListener() const { return block_fn_ != nullptr; }

    struct Stats {
        uint32_t in_packets, in_errors, in_missed, in_late, in_overruns;
        uint32_t out_packets, out_errors, out_missed, out_late, out_underruns;
        uint32_t feedback_packets, feedback_ignored;
        uint32_t rate_q16; // paced output frames per packet, 16.16
    };
    Stats stats() const;
    bool explicitFeedback() const { return fb_active_; }
    bool implicitFeedback() const { return implicit_; }

    // Prints the function, the chosen formats and the counters.
    void printStatus(Print& out) const;

protected:
    virtual bool claim(Device_t* device, int type, const uint8_t* descriptors, uint32_t len);
    virtual void control(const Transfer_t* transfer);
    virtual void timer_event(USBDriverTimer* timer);
    virtual void Task();
    virtual void disconnect();

private:
    enum Step : uint8_t {
        STEP_NONE,
        STEP_GET_RANGE,
        STEP_SET_SELECTOR,
        STEP_SET_RATE,
        STEP_GET_RATE,
        STEP_SET_IFACE_IN,
        STEP_SET_RATE_IN,
        STEP_SET_IFACE_OUT,
        STEP_SET_RATE_OUT,
        STEP_START,
        STEP_DONE
    };

    void init();
    void fail(const char* why);
    void advance();
    bool issue(Step step);
    bool send(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length);
    void chooseRateFromRange();
    void startStreams();
    void stopStreams();

    static void inReceiveThunk(void* ctx, const uint8_t* data, uint16_t len, bool ok);
    static uint16_t outProduceThunk(void* ctx, uint8_t* data, uint16_t capacity);
    static void fbReceiveThunk(void* ctx, const uint8_t* data, uint16_t len, bool ok);
    void inReceive(const uint8_t* data, uint16_t len, bool ok);
    uint16_t outProduce(uint8_t* data, uint16_t capacity);

    // Wanted
    uint32_t wanted_rate_ = 44100;
    uint8_t wanted_in_ = 2;
    uint8_t wanted_out_ = 2;

    // Negotiated
    usb_audio_host::AudioFunction function_;
    const usb_audio_host::Format* in_format_ = nullptr;
    const usb_audio_host::Format* out_format_ = nullptr;
    usb_audio_host::ClockPath clock_;
    bool clock_programmable_ = true;
    uint32_t rate_ = 0;
    bool high_speed_ = false;
    bool implicit_ = false;
    volatile bool fb_active_ = false;

    // Control plane
    volatile State state_ = State::Idle;
    const char* failure_ = "";
    Step step_ = STEP_NONE;
    uint8_t retries_ = 0;
    setup_t setup_ = {};
    uint8_t control_buf_[2 + 12 * USB_AUDIO_HOST_MAX_RATES] __attribute__((aligned(4))) = {};
    USBDriverTimer timer_;
    Transfer_t transfers_[6] __attribute__((aligned(32))) = {};

    // Streams
    usb_audio_host::IsoStream in_stream_;
    usb_audio_host::IsoStream out_stream_;
    usb_audio_host::IsoStream fb_stream_;
    usb_audio_host::IsoDescriptor in_desc_[USB_AUDIO_HOST_QUEUE_FRAMES];
    usb_audio_host::IsoDescriptor out_desc_[USB_AUDIO_HOST_QUEUE_FRAMES];
    usb_audio_host::IsoDescriptor fb_desc_[USB_AUDIO_HOST_QUEUE_FRAMES];
    uint8_t in_buf_[USB_AUDIO_HOST_QUEUE_FRAMES * 8 * USB_AUDIO_HOST_PACKET_BYTES] __attribute__((aligned(32)));
    uint8_t out_buf_[USB_AUDIO_HOST_QUEUE_FRAMES * 8 * USB_AUDIO_HOST_PACKET_BYTES] __attribute__((aligned(32)));
    uint8_t fb_buf_[USB_AUDIO_HOST_QUEUE_FRAMES * 8 * 32] __attribute__((aligned(32)));
    usb_audio_host::Pacer pacer_;
    usb_audio_host::SampleRing<USB_AUDIO_HOST_RING_FRAMES, USB_AUDIO_HOST_MAX_CHANNELS> in_ring_;
    usb_audio_host::SampleRing<USB_AUDIO_HOST_RING_FRAMES, USB_AUDIO_HOST_MAX_CHANNELS> out_ring_;
    uint8_t in_frame_bytes_ = 0;
    uint8_t out_frame_bytes_ = 0;
    volatile uint32_t feedback_packets_ = 0;

    // Block clock
    BlockFn block_fn_ = nullptr;
    void* block_ctx_ = nullptr;
    uint16_t block_frames_ = 0;
    volatile bool block_pending_ = false;
};

#endif
