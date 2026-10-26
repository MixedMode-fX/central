#ifndef USB_AUDIO_HOST_ISO_ENGINE_H
#define USB_AUDIO_HOST_ISO_ENGINE_H

#include <stdint.h>
#include "usb_audio_host/config.h"

// Isochronous transfers for USBHost_t36, which has none.
//
// USBHost_t36 owns the EHCI controller: it built the periodic frame list,
// put its interrupt queue heads in it and services its interrupt. This
// engine adds isochronous transfer descriptors to that same frame list - an
// iTD per frame for a high-speed endpoint (EHCI 3.3), an siTD per frame for a
// full-speed one (EHCI 3.4) - ahead of the queue heads, where the EHCI
// specification wants them, and chains itself in front of USBHost_t36's
// interrupt handler to harvest and re-arm them.
//
// Two facts about USBHost_t36 are relied on, and both are checked at compile
// time or run time rather than assumed: its frame list base is what the
// controller's PERIODICLISTBASE register says, sized as USBCMD says; and its
// code that walks a frame's list casts every node to its Pipe_t and reads
// `periodic_interval` at a fixed offset to decide where a new queue head
// goes. Each descriptor here carries that field at that offset, holding a
// value larger than any interval, so the walker steps over isochronous
// descriptors and keeps inserting queue heads after them.
//
// A stream keeps USB_AUDIO_HOST_QUEUE_FRAMES descriptors queued in the frames
// ahead of the controller. As each one completes it is unlinked, its packets
// delivered (IN) or refilled (OUT), and it is linked again at the far end of
// the window. Packet buffers and descriptors live in ordinary RAM, which on
// the Teensy 4 is uncached DTCM: no cache maintenance is needed, and none is
// done, so do not place a USBAudioDevice in DMAMEM or EXTMEM.
namespace usb_audio_host {

class IsoStream;

// One transfer descriptor: 64 bytes the controller reads, then bookkeeping.
struct __attribute__((aligned(32))) IsoDescriptor {
    volatile uint32_t link;      // next link pointer: address | type<<1 | terminate
    volatile uint32_t hw[15];    // iTD: 8 transactions + 7 page pointers; siTD: 6 words used
    uint32_t reserved;           // offset 64
    uint16_t periodic_interval;  // offset 68: what USBHost_t36's list walker reads
    uint16_t frame;              // slot it is linked in, or NO_FRAME
    uint8_t* data;               // its packet buffer
    IsoStream* stream;           // owner
    uint16_t length[8];          // bytes queued per packet (OUT) or received (IN)

    enum { NO_FRAME = 0xFFFF };
    bool linked() const { return frame != NO_FRAME; }
};

class IsoStream {
public:
    struct Config {
        uint8_t device_address = 0;
        uint8_t hub_address = 0; // 0 when on the root port
        uint8_t hub_port = 0;
        bool high_speed = false; // iTD; otherwise siTD through the (embedded) transaction translator
        uint8_t endpoint = 0;    // endpoint number, 1..15
        bool in = false;
        uint16_t max_packet = 0; // wMaxPacketSize bits 10:0
        uint8_t mult = 1;        // transactions per microframe, 1..3
        uint8_t interval = 1;    // bInterval
    };

    // IN: a packet arrived; `ok` is false when the controller flagged an
    // error on it, in which case `length` is 0.
    typedef void (*ReceiveFn)(void* context, const uint8_t* data, uint16_t length, bool ok);
    // OUT: fill `data` with at most `capacity` bytes; return how many.
    typedef uint16_t (*ProduceFn)(void* context, uint8_t* data, uint16_t capacity);

    IsoStream();

    // Memory the stream works in: `count` descriptors and a buffer that is
    // split evenly between them.
    void attach(IsoDescriptor* descriptors, uint8_t count, uint8_t* buffer, uint32_t buffer_bytes);
    void onReceive(ReceiveFn fn, void* context) { receive_ = fn; receive_context_ = context; }
    void onProduce(ProduceFn fn, void* context) { produce_ = fn; produce_context_ = context; }

    // Lays the schedule out for an endpoint; false when its packets do not
    // fit the attached buffer. The stream must be stopped.
    bool configure(const Config& config);

    // Links the first window of descriptors. If the stream was stopped less
    // than two frames ago the start is deferred to the next service() so the
    // controller is done with the old descriptors.
    bool start();
    void stop();
    bool running() const { return running_; }

    // Harvests completed descriptors and re-arms them. Called by the engine
    // from the USB interrupt and, with that interrupt masked, from Task().
    void service();

    uint32_t packetsPerSecond() const { return packets_per_second_; }
    uint8_t packetsPerFrame() const { return packets_per_frame_; }
    uint16_t packetCapacity() const { return capacity_; }
    bool highSpeed() const { return high_speed_; }

    uint32_t packets() const { return packets_; }
    uint32_t errors() const { return errors_; }
    uint32_t missed() const { return missed_; }
    uint32_t late() const { return late_; }

private:
    void startNow();
    void arm(IsoDescriptor& d);
    void armHigh(IsoDescriptor& d);
    void armFull(IsoDescriptor& d);
    bool complete(const IsoDescriptor& d) const;
    void harvest(IsoDescriptor& d);
    void deactivate(IsoDescriptor& d);

    IsoDescriptor* desc_ = nullptr;
    uint8_t count_ = 0;        // descriptors attached
    uint8_t active_count_ = 0; // descriptors the window uses: fewer when frames are skipped
    uint8_t* buffer_ = nullptr;
    uint32_t buffer_bytes_ = 0;
    uint32_t bytes_per_descriptor_ = 0;

    ReceiveFn receive_ = nullptr;
    void* receive_context_ = nullptr;
    ProduceFn produce_ = nullptr;
    void* produce_context_ = nullptr;

    uint8_t address_ = 0;
    uint8_t hub_ = 0;
    uint8_t port_ = 0;
    uint8_t endpoint_ = 0;
    bool in_ = false;
    bool high_speed_ = false;
    uint16_t max_packet_ = 0;
    uint8_t mult_ = 1;
    uint16_t capacity_ = 0; // bytes per packet
    uint16_t stride_ = 0;   // bytes between packet buffers
    uint8_t packets_per_frame_ = 1;
    uint8_t frame_stride_ = 1;
    uint32_t packets_per_second_ = 0;

    volatile bool running_ = false;
    volatile bool start_pending_ = false;
    uint8_t head_ = 0;
    uint16_t next_frame_ = 0;
    uint16_t stop_frame_ = 0;
    volatile uint32_t packets_ = 0;
    volatile uint32_t errors_ = 0;
    volatile uint32_t missed_ = 0;
    volatile uint32_t late_ = 0;
};

// The shared half: the frame list, the interrupt hook, the set of streams.
class IsoEngine {
public:
    // Chains in front of USBHost_t36's interrupt handler. Call once the
    // host has begun; it is safe from inside that interrupt, and repeated
    // calls do nothing.
    static void install();
    static bool installed() { return installed_; }

    static void add(IsoStream* stream);
    static void remove(IsoStream* stream);

    // Services every stream. From the USB interrupt via the hook; from a
    // driver's Task() through serviceMasked().
    static void service();
    static void serviceMasked();

    static uint32_t* frameList();
    static uint32_t frameListSize();
    static uint32_t frameMask() { return frameListSize() - 1; }
    static uint32_t frameNow();
    static void link(IsoDescriptor& d, uint16_t slot, bool sitd);
    static void unlink(IsoDescriptor& d);

    enum { MAX_STREAMS = 6 };

private:
    static void isr();
    static bool installed_;
    static void (*chained_)(void);
    static IsoStream* streams_[MAX_STREAMS];
};

} // namespace usb_audio_host

#endif
