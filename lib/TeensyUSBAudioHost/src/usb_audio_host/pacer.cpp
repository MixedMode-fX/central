#include "usb_audio_host/pacer.h"

namespace usb_audio_host {

void Pacer::begin(uint32_t rate, uint32_t packets_per_second)
{
    rate_ = rate;
    packets_per_second_ = packets_per_second ? packets_per_second : 1;
    remainder_ = 0;
    feedback_q16_ = 0;
    accumulator_q16_ = 0;
    feedback_shift_ = 127;
    feedback_ignored_ = 0;
    mirror_head_ = mirror_tail_ = 0;
}

uint32_t Pacer::nominalQ16() const
{
    return static_cast<uint32_t>((static_cast<uint64_t>(rate_) << 16) / packets_per_second_);
}

uint16_t Pacer::next()
{
    if (mirror_head_ != mirror_tail_) {
        const uint16_t frames = mirror_[mirror_tail_];
        mirror_tail_ = static_cast<uint8_t>((mirror_tail_ + 1) % MIRROR_DEPTH);
        return frames;
    }
    if (feedback_q16_) {
        accumulator_q16_ += feedback_q16_;
        const uint16_t frames = static_cast<uint16_t>(accumulator_q16_ >> 16);
        accumulator_q16_ &= 0xFFFF;
        return frames;
    }
    remainder_ += rate_;
    const uint16_t frames = static_cast<uint16_t>(remainder_ / packets_per_second_);
    remainder_ %= packets_per_second_;
    return frames;
}

void Pacer::mirror(uint16_t frames)
{
    const uint8_t next_head = static_cast<uint8_t>((mirror_head_ + 1) % MIRROR_DEPTH);
    if (next_head == mirror_tail_) {
        // The sink is not draining: keep the newest, forget the oldest.
        mirror_tail_ = static_cast<uint8_t>((mirror_tail_ + 1) % MIRROR_DEPTH);
    }
    mirror_[mirror_head_] = frames;
    mirror_head_ = next_head;
}

// The feedback value is frames per frame (10.14, full speed) or frames per
// microframe (16.16, high speed). Either way it is a rate in 16.16 after a
// shift of 2 for the 10.14 case; what differs is the period it refers to,
// and devices get that wrong, so the shift that lands the value within
// -25%/+50% of the nominal per-packet rate is found once and then kept.
void Pacer::feedback(const uint8_t* payload, uint8_t length)
{
    const uint32_t nominal = nominalQ16();
    if (length < 3 || nominal == 0) return;
    uint32_t f = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8) |
                 (static_cast<uint32_t>(payload[2]) << 16);
    if (length >= 4) f |= static_cast<uint32_t>(payload[3] & 0x0F) << 24;
    if (f == 0) {
        feedback_ignored_++;
        return;
    }
    const uint32_t low = nominal - nominal / 4;
    const uint32_t high = nominal + nominal / 2;
    if (feedback_shift_ == 127) {
        int8_t shift = 0;
        uint32_t v = f;
        while (v < low && shift < 16) { v <<= 1; shift++; }
        while (v > high && shift > -16) { v >>= 1; shift--; }
        if (v < low || v > high) {
            feedback_ignored_++;
            return;
        }
        feedback_shift_ = shift;
    }
    const uint32_t value = feedback_shift_ >= 0 ? (f << feedback_shift_) : (f >> -feedback_shift_);
    // A device that drifts more than 1% from nominal is not reporting a
    // rate; its value is dropped rather than paced to.
    const uint32_t tolerance = nominal / 128;
    if (value < nominal - tolerance || value > nominal + tolerance) {
        feedback_ignored_++;
        return;
    }
    feedback_q16_ = value;
}

} // namespace usb_audio_host
