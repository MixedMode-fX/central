#ifndef USB_AUDIO_HOST_PACER_H
#define USB_AUDIO_HOST_PACER_H

#include <stdint.h>

// Decides how many frames each outgoing isochronous packet carries.
//
// The nominal rate is paced exactly: a remainder of frames carried from
// packet to packet, so 44.1 kHz over 8 kHz microframes becomes a run of 5s
// and 6s that sums to 44100 every second. An asynchronous sink corrects it
// two ways: an explicit feedback endpoint reports its rate (UAC 3.7.2.2:
// 10.14 frames per frame on full speed, 16.16 frames per microframe on high
// speed, with the two mistaken for each other often enough that the format
// is detected rather than trusted, as Linux does), or implicit feedback
// mirrors the packet sizes an IN endpoint on the same clock just delivered.
//
// Framework-free; unit-tested in test/test_pacer.
namespace usb_audio_host {

class Pacer {
public:
    Pacer() = default;

    // rate in Hz; packets per second this endpoint takes (8000 >> (bInterval-1)
    // on high speed, 1000 / bInterval on full speed).
    void begin(uint32_t rate, uint32_t packets_per_second);

    // Frames for the next packet.
    uint16_t next();

    // Explicit feedback: the raw little-endian payload of the feedback
    // endpoint and its length (3 or 4 bytes). Values that do not resemble
    // the nominal rate under any format are ignored.
    void feedback(const uint8_t* payload, uint8_t length);

    // Implicit feedback: a packet of `frames` frames arrived on the source
    // endpoint, to be mirrored by the next outgoing packet.
    void mirror(uint16_t frames);

    // Frames per packet in 16.16 fixed point, as currently paced.
    uint32_t rateQ16() const { return feedback_q16_ ? feedback_q16_ : nominalQ16(); }
    uint32_t nominalQ16() const;
    uint32_t feedbackIgnored() const { return feedback_ignored_; }
    bool feedbackActive() const { return feedback_q16_ != 0; }

    enum { MIRROR_DEPTH = 32 };

private:
    uint32_t rate_ = 0;
    uint32_t packets_per_second_ = 1;
    uint32_t remainder_ = 0;      // frames owed, in units of 1/packets_per_second
    uint32_t feedback_q16_ = 0;   // 0 until a feedback value is accepted
    uint32_t accumulator_q16_ = 0;
    int8_t feedback_shift_ = 127; // unknown until the first value
    uint32_t feedback_ignored_ = 0;
    uint16_t mirror_[MIRROR_DEPTH] = {};
    uint8_t mirror_head_ = 0;
    uint8_t mirror_tail_ = 0;
};

} // namespace usb_audio_host

#endif
