#if defined(__MK66FX1M0__) || defined(__IMXRT1052__) || defined(__IMXRT1062__)

#include <Arduino.h>
#include <USBHost_t36.h>
#include <stddef.h>
#include <string.h>
#include "usb_audio_host/iso_engine.h"

namespace usb_audio_host {

// USBHost_t36's add_qh_to_periodic_schedule() and delete_Pipe() walk a
// frame's list as a chain of Pipe_t, reading periodic_interval and the link
// word at offset 0. Both are here at the same offsets.
static_assert(sizeof(IsoDescriptor) == 96, "IsoDescriptor must stay 96 bytes");
static_assert(offsetof(IsoDescriptor, link) == offsetof(Pipe_t, qh.horizontal_link),
              "the link word must sit where USBHost_t36 reads a queue head's horizontal link");
static_assert(offsetof(IsoDescriptor, periodic_interval) == offsetof(Pipe_t, periodic_interval),
              "periodic_interval must sit where USBHost_t36's periodic list walker reads it");
static_assert(USB_AUDIO_HOST_QUEUE_FRAMES >= 2, "at least two frames must be queued");

// EHCI link type field (bits 2:1): 0 iTD, 1 QH, 2 siTD, 3 FSTN.
static const uint32_t LINK_TERMINATE = 1;
static const uint32_t LINK_TYPE_MASK = 6;
static const uint32_t LINK_TYPE_QH = 2;
static const uint32_t LINK_TYPE_SITD = 4;
static const uint32_t LINK_ADDRESS_MASK = 0xFFFFFFE0;

static const uint16_t INTERVAL_SHADOW = 0xFFFF; // larger than any interrupt pipe's

static inline void barrier() { asm volatile("dsb" ::: "memory"); }

/* ------------------------------------------------------------------------- */
/* IsoEngine                                                                 */
/* ------------------------------------------------------------------------- */

bool IsoEngine::installed_ = false;
void (*IsoEngine::chained_)(void) = nullptr;
IsoStream* IsoEngine::streams_[IsoEngine::MAX_STREAMS] = {};

uint32_t* IsoEngine::frameList()
{
    return reinterpret_cast<uint32_t*>(USBHS_PERIODICLISTBASE & 0xFFFFF000);
}

// USBCMD frame list size: FS2:FS1:FS0 selects 1024 >> n entries.
uint32_t IsoEngine::frameListSize()
{
    const uint32_t cmd = USBHS_USBCMD;
    const uint32_t n = ((cmd & USBHS_USBCMD_FS2) ? 4 : 0) | ((cmd >> 2) & 3);
    return 1024u >> n;
}

uint32_t IsoEngine::frameNow()
{
    return (USBHS_FRINDEX >> 3) & frameMask();
}

void IsoEngine::install()
{
    if (installed_) return;
    void (*current)(void) = _VectorsRam[IRQ_USBHS + 16];
    if (current != isr) {
        chained_ = current;
        attachInterruptVector(IRQ_USBHS, isr);
    }
    installed_ = true;
}

void IsoEngine::isr()
{
    service();
    if (chained_) chained_();
}

void IsoEngine::add(IsoStream* stream)
{
    for (IsoStream*& s : streams_) {
        if (s == stream) return;
    }
    for (IsoStream*& s : streams_) {
        if (s == nullptr) {
            s = stream;
            return;
        }
    }
}

void IsoEngine::remove(IsoStream* stream)
{
    for (IsoStream*& s : streams_) {
        if (s == stream) s = nullptr;
    }
}

void IsoEngine::service()
{
    for (IsoStream* s : streams_) {
        if (s) s->service();
    }
}

void IsoEngine::serviceMasked()
{
    const bool enabled = NVIC_IS_ENABLED(IRQ_USBHS);
    NVIC_DISABLE_IRQ(IRQ_USBHS);
    service();
    if (enabled) NVIC_ENABLE_IRQ(IRQ_USBHS);
}

// Head insertion keeps every isochronous descriptor ahead of the queue
// heads in its frame, which is both what the EHCI specification asks and
// what keeps USBHost_t36's walker from ever inserting in front of one.
void IsoEngine::link(IsoDescriptor& d, uint16_t slot, bool sitd)
{
    uint32_t* list = frameList();
    d.link = list[slot];
    barrier();
    list[slot] = reinterpret_cast<uint32_t>(&d) | (sitd ? LINK_TYPE_SITD : 0);
    barrier();
    d.frame = slot;
}

void IsoEngine::unlink(IsoDescriptor& d)
{
    if (!d.linked()) return;
    volatile uint32_t* p = &frameList()[d.frame];
    for (int guard = 0; guard < 64; guard++) {
        const uint32_t v = *p;
        if (v & LINK_TERMINATE) break;
        if ((v & LINK_TYPE_MASK) == LINK_TYPE_QH) break; // past our segment: not here
        IsoDescriptor* node = reinterpret_cast<IsoDescriptor*>(v & LINK_ADDRESS_MASK);
        if (node == &d) {
            *p = d.link;
            barrier();
            break;
        }
        p = &node->link;
    }
    d.frame = IsoDescriptor::NO_FRAME;
}

/* ------------------------------------------------------------------------- */
/* IsoStream                                                                 */
/* ------------------------------------------------------------------------- */

IsoStream::IsoStream() {}

void IsoStream::attach(IsoDescriptor* descriptors, uint8_t count, uint8_t* buffer, uint32_t buffer_bytes)
{
    desc_ = descriptors;
    count_ = count;
    buffer_ = buffer;
    buffer_bytes_ = buffer_bytes;
    for (uint8_t i = 0; i < count_; i++) {
        memset(&desc_[i], 0, sizeof(IsoDescriptor));
        desc_[i].link = LINK_TERMINATE;
        desc_[i].periodic_interval = INTERVAL_SHADOW;
        desc_[i].frame = IsoDescriptor::NO_FRAME;
        desc_[i].stream = this;
    }
}

bool IsoStream::configure(const Config& c)
{
    if (running_ || !desc_ || count_ == 0 || !buffer_) return false;
    address_ = c.device_address;
    hub_ = c.hub_address;
    port_ = c.hub_port;
    endpoint_ = c.endpoint & 0x0F;
    in_ = c.in;
    high_speed_ = c.high_speed;
    max_packet_ = c.max_packet;
    mult_ = c.mult < 1 ? 1 : (c.mult > 3 ? 3 : c.mult);
    capacity_ = static_cast<uint16_t>(max_packet_ * mult_);
    stride_ = static_cast<uint16_t>((capacity_ + 31) & ~31u);
    if (capacity_ == 0) return false;
    if (high_speed_) {
        uint8_t interval = c.interval < 1 ? 1 : (c.interval > 16 ? 16 : c.interval);
        if (interval <= 4) {
            packets_per_frame_ = static_cast<uint8_t>(8 >> (interval - 1));
            frame_stride_ = 1;
        } else {
            packets_per_frame_ = 1;
            frame_stride_ = static_cast<uint8_t>(1 << (interval - 4));
        }
        packets_per_second_ = 8000u >> (interval - 1);
    } else {
        if (capacity_ > 1023) return false; // siTD total bytes is a 10-bit field
        if (mult_ != 1) return false;
        packets_per_frame_ = 1;
        frame_stride_ = c.interval < 1 ? 1 : c.interval;
        packets_per_second_ = 1000u / frame_stride_;
    }
    if (packets_per_second_ == 0) return false;
    // The window must stay well inside the frame list: the controller walks
    // it circularly and a descriptor linked too far ahead is behind. A
    // stream that skips frames uses fewer descriptors to keep it so.
    const uint32_t half = IsoEngine::frameListSize() / 2;
    active_count_ = count_;
    while (active_count_ > 1 && static_cast<uint32_t>(active_count_) * frame_stride_ + 2 > half) active_count_--;
    if (static_cast<uint32_t>(active_count_) * frame_stride_ + 2 > half) return false;
    bytes_per_descriptor_ = static_cast<uint32_t>(packets_per_frame_) * stride_;
    if (bytes_per_descriptor_ * count_ > buffer_bytes_) return false;
    if (high_speed_ && bytes_per_descriptor_ > 6u * 4096u) return false; // 7 page pointers
    for (uint8_t i = 0; i < count_; i++) {
        desc_[i].data = buffer_ + i * bytes_per_descriptor_;
    }
    return true;
}

bool IsoStream::start()
{
    if (running_ || packets_per_second_ == 0) return running_;
    const bool enabled = NVIC_IS_ENABLED(IRQ_USBHS);
    NVIC_DISABLE_IRQ(IRQ_USBHS);
    const uint32_t since_stop = (IsoEngine::frameNow() - stop_frame_) & IsoEngine::frameMask();
    if (since_stop < 2) {
        start_pending_ = true;
    } else {
        startNow();
    }
    if (enabled) NVIC_ENABLE_IRQ(IRQ_USBHS);
    return true;
}

void IsoStream::startNow()
{
    const uint32_t mask = IsoEngine::frameMask();
    packets_ = errors_ = missed_ = late_ = 0;
    next_frame_ = static_cast<uint16_t>((IsoEngine::frameNow() + 2) & mask);
    for (uint8_t i = 0; i < active_count_; i++) {
        arm(desc_[i]);
        IsoEngine::link(desc_[i], next_frame_, !high_speed_);
        next_frame_ = static_cast<uint16_t>((next_frame_ + frame_stride_) & mask);
    }
    head_ = 0;
    start_pending_ = false;
    running_ = true;
}

void IsoStream::stop()
{
    const bool enabled = NVIC_IS_ENABLED(IRQ_USBHS);
    NVIC_DISABLE_IRQ(IRQ_USBHS);
    start_pending_ = false;
    if (running_) {
        for (uint8_t i = 0; i < count_; i++) {
            deactivate(desc_[i]);
            IsoEngine::unlink(desc_[i]);
        }
        running_ = false;
        stop_frame_ = static_cast<uint16_t>(IsoEngine::frameNow());
    }
    if (enabled) NVIC_ENABLE_IRQ(IRQ_USBHS);
}

void IsoStream::service()
{
    if (!running_) {
        if (start_pending_) {
            const uint32_t since_stop = (IsoEngine::frameNow() - stop_frame_) & IsoEngine::frameMask();
            if (since_stop >= 2) startNow();
        }
        return;
    }
    const uint32_t mask = IsoEngine::frameMask();
    const uint32_t half = IsoEngine::frameListSize() / 2;
    for (uint8_t guard = 0; guard < active_count_; guard++) {
        IsoDescriptor& d = desc_[head_];
        if (!d.linked()) break;
        const uint32_t now = IsoEngine::frameNow();
        const uint32_t since = (now - d.frame) & mask;
        if (!complete(d)) {
            // Its frame has gone by and the controller left it active: it
            // was skipped. Reclaim it rather than wait a full list cycle.
            if (since >= 2 && since < half) {
                missed_++;
                deactivate(d);
            } else {
                break;
            }
        }
        IsoEngine::unlink(d);
        harvest(d);
        uint32_t target = next_frame_;
        const uint32_t ahead = (target - now) & mask;
        if (ahead < 2 || ahead >= half) {
            // We fell behind by more than the window: resume ahead of the
            // controller and count the gap.
            target = (now + 2) & mask;
            late_++;
        }
        arm(d);
        IsoEngine::link(d, static_cast<uint16_t>(target), !high_speed_);
        next_frame_ = static_cast<uint16_t>((target + frame_stride_) & mask);
        head_ = static_cast<uint8_t>((head_ + 1) % active_count_);
    }
}

void IsoStream::arm(IsoDescriptor& d)
{
    if (high_speed_) armHigh(d);
    else armFull(d);
}

// iTD, EHCI 3.3. Transaction k of the frame runs in microframe k * step and
// points into page PG at offset; a packet that crosses into the next page is
// followed there by the controller. The last transaction raises the
// interrupt.
void IsoStream::armHigh(IsoDescriptor& d)
{
    const uint32_t base = reinterpret_cast<uint32_t>(d.data) & 0xFFFFF000u;
    for (int p = 0; p < 7; p++) d.hw[8 + p] = base + static_cast<uint32_t>(p) * 0x1000u;
    d.hw[8] |= (static_cast<uint32_t>(endpoint_) << 8) | address_;
    d.hw[9] |= (in_ ? (1u << 11) : 0u) | max_packet_;
    d.hw[10] |= mult_;
    const uint8_t step = static_cast<uint8_t>(8 / packets_per_frame_);
    for (int i = 0; i < 8; i++) d.hw[i] = 0;
    for (uint8_t k = 0; k < packets_per_frame_; k++) {
        uint8_t* p = d.data + k * stride_;
        uint16_t len = capacity_;
        if (!in_) {
            len = produce_ ? produce_(produce_context_, p, capacity_) : 0;
            if (len > capacity_) len = capacity_;
        }
        d.length[k] = len;
        const uint32_t addr = reinterpret_cast<uint32_t>(p);
        const uint32_t pg = (addr - base) >> 12;
        const uint32_t off = addr & 0xFFFu;
        const bool last = (k + 1 == packets_per_frame_);
        d.hw[k * step] = (1u << 31) | (static_cast<uint32_t>(len) << 16) | (last ? (1u << 15) : 0u) |
                         (pg << 12) | off;
    }
    barrier();
}

// siTD, EHCI 3.4, one full-speed packet per frame through a transaction
// translator - a hub's, or the controller's own for a device on the root
// port, which takes hub address and port 0. Masks follow USB 2.0 11.18.4 as
// Linux lays them out: IN issues its start-split in microframe 0 and
// collects complete-splits from microframe 2 for as many 188-byte chunks as
// the packet may hold, plus two; OUT sends one start-split per chunk from
// microframe 0.
void IsoStream::armFull(IsoDescriptor& d)
{
    uint16_t len = capacity_;
    if (!in_) {
        len = produce_ ? produce_(produce_context_, d.data, capacity_) : 0;
        if (len > capacity_) len = capacity_;
    }
    d.length[0] = len;
    uint32_t chunks = (len + 187u) / 188u;
    if (chunks < 1) chunks = 1;
    if (chunks > 5) chunks = 5;
    const uint32_t smask = in_ ? 0x01u : ((1u << chunks) - 1u);
    const uint32_t cmask = in_ ? ((((1u << (chunks + 2)) - 1u) << 2) & 0xFFu) : 0u;
    const uint32_t addr = reinterpret_cast<uint32_t>(d.data);
    d.hw[0] = (in_ ? (1u << 31) : 0u) | (static_cast<uint32_t>(port_) << 24) |
              (static_cast<uint32_t>(hub_) << 16) | (static_cast<uint32_t>(endpoint_) << 8) | address_;
    d.hw[1] = (cmask << 8) | smask;
    d.hw[2] = (1u << 31) | (static_cast<uint32_t>(len) << 16) | 0x80u;
    d.hw[3] = addr;
    d.hw[4] = ((addr + len) & 0xFFFFF000u) | (in_ ? 0u : (chunks | (chunks > 1 ? (1u << 3) : 0u)));
    d.hw[5] = LINK_TERMINATE;
    for (int i = 6; i < 15; i++) d.hw[i] = 0;
    barrier();
}

bool IsoStream::complete(const IsoDescriptor& d) const
{
    if (high_speed_) {
        const uint8_t step = static_cast<uint8_t>(8 / packets_per_frame_);
        for (uint8_t k = 0; k < packets_per_frame_; k++) {
            if (d.hw[k * step] & (1u << 31)) return false;
        }
        return true;
    }
    return (d.hw[2] & 0x80u) == 0;
}

void IsoStream::deactivate(IsoDescriptor& d)
{
    if (high_speed_) {
        for (int i = 0; i < 8; i++) d.hw[i] &= ~(1u << 31);
    } else {
        d.hw[2] &= ~0x80u;
    }
    barrier();
}

void IsoStream::harvest(IsoDescriptor& d)
{
    if (high_speed_) {
        const uint8_t step = static_cast<uint8_t>(8 / packets_per_frame_);
        for (uint8_t k = 0; k < packets_per_frame_; k++) {
            const uint32_t t = d.hw[k * step];
            const uint32_t status = (t >> 28) & 0xFu;
            const bool ok = (status & 0x7u) == 0;
            packets_++;
            if (!ok) errors_++;
            if (in_) {
                const uint16_t len = ok ? static_cast<uint16_t>((t >> 16) & 0xFFFu) : 0;
                d.length[k] = len;
                if (receive_) receive_(receive_context_, d.data + k * stride_, len, ok);
            }
        }
    } else {
        const uint32_t t = d.hw[2];
        const bool ok = (t & 0x7Cu) == 0;
        packets_++;
        if (!ok) errors_++;
        if (in_) {
            const uint32_t remaining = (t >> 16) & 0x3FFu;
            const uint16_t len = (ok && remaining <= d.length[0]) ? static_cast<uint16_t>(d.length[0] - remaining) : 0;
            d.length[0] = len;
            if (receive_) receive_(receive_context_, d.data, len, ok);
        }
    }
}

} // namespace usb_audio_host

#endif
