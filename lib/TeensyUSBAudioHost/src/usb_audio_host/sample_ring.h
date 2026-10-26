#ifndef USB_AUDIO_HOST_SAMPLE_RING_H
#define USB_AUDIO_HOST_SAMPLE_RING_H

#include <stdint.h>

// A single-producer, single-consumer ring of interleaved frames: one writer
// (the USB interrupt on the way in, the sketch on the way out), one reader,
// no locks. Each side owns its own index; the other side only reads it. Any
// number of channels up to the ring's width, fixed when the stream starts.
//
// Samples are int32_t, left-justified: a 16-bit device sample occupies the
// top 16 bits, a 24-bit one the top 24.
//
// Framework-free; unit-tested in test/test_sample_ring.
namespace usb_audio_host {

template <uint32_t FRAMES, uint8_t MAX_CHANNELS>
class SampleRing {
public:
    SampleRing() = default;

    void begin(uint8_t channels)
    {
        channels_ = (channels == 0) ? 1 : (channels > MAX_CHANNELS ? MAX_CHANNELS : channels);
        head_ = tail_ = 0;
        overruns_ = underruns_ = 0;
    }

    uint8_t channels() const { return channels_; }
    uint32_t capacity() const { return FRAMES - 1; }

    uint32_t available() const
    {
        const uint32_t h = head_, t = tail_;
        return h >= t ? h - t : FRAMES - t + h;
    }
    uint32_t space() const { return capacity() - available(); }

    // Writes up to `frames` frames; returns how many fit. The rest is
    // counted as an overrun.
    uint32_t write(const int32_t* src, uint32_t frames)
    {
        const uint32_t n = frames > space() ? space() : frames;
        if (n < frames) overruns_ += frames - n;
        uint32_t h = head_;
        for (uint32_t i = 0; i < n; i++) {
            int32_t* dst = &data_[h * MAX_CHANNELS];
            for (uint8_t c = 0; c < channels_; c++) dst[c] = *src++;
            h = (h + 1 == FRAMES) ? 0 : h + 1;
        }
        head_ = h;
        return n;
    }

    // Reads up to `frames` frames; returns how many there were. The
    // remainder of `dst` is zero-filled and counted as an underrun.
    uint32_t read(int32_t* dst, uint32_t frames)
    {
        const uint32_t n = frames > available() ? available() : frames;
        if (n < frames) underruns_ += frames - n;
        uint32_t t = tail_;
        for (uint32_t i = 0; i < n; i++) {
            const int32_t* src = &data_[t * MAX_CHANNELS];
            for (uint8_t c = 0; c < channels_; c++) *dst++ = src[c];
            t = (t + 1 == FRAMES) ? 0 : t + 1;
        }
        tail_ = t;
        for (uint32_t i = n * channels_; i < frames * channels_; i++) *dst++ = 0;
        return n;
    }

    // Drops frames from the read side, oldest first.
    uint32_t discard(uint32_t frames)
    {
        const uint32_t n = frames > available() ? available() : frames;
        tail_ = (tail_ + n) % FRAMES;
        return n;
    }

    // One frame's channel at a time, for writers that convert as they go.
    // Call beginFrame(), set channels with sample(), then endFrame().
    bool beginFrame()
    {
        if (space() == 0) {
            overruns_++;
            return false;
        }
        return true;
    }
    void sample(uint8_t channel, int32_t value) { data_[head_ * MAX_CHANNELS + channel] = value; }
    void endFrame() { head_ = (head_ + 1 == FRAMES) ? 0 : head_ + 1; }

    uint32_t overruns() const { return overruns_; }
    uint32_t underruns() const { return underruns_; }

private:
    int32_t data_[FRAMES * MAX_CHANNELS] = {};
    volatile uint32_t head_ = 0;
    volatile uint32_t tail_ = 0;
    uint8_t channels_ = 1;
    volatile uint32_t overruns_ = 0;
    volatile uint32_t underruns_ = 0;
};

} // namespace usb_audio_host

#endif
