#ifndef USB_AUDIO_HOST_CONFIG_H
#define USB_AUDIO_HOST_CONFIG_H

// Sizing. Every constant can be overridden from the build
// (-D USB_AUDIO_HOST_MAX_CHANNELS=4). Memory is allocated statically inside
// the USBAudioDevice object from these numbers, so a sketch that only needs a
// stereo pair at 48 kHz can shrink them considerably.

// Channels per direction the device object can carry. A device offering more
// is streamed with the alternate setting whose channel count fits best; the
// surplus channels of a wider setting are received and discarded.
#ifndef USB_AUDIO_HOST_MAX_CHANNELS
#define USB_AUDIO_HOST_MAX_CHANNELS 8
#endif

// Depth of the sample rings between the USB engine and the sketch, in frames
// (one frame = one sample per channel). Latency is governed by how much the
// reader lets accumulate, not by this number; it only bounds it.
#ifndef USB_AUDIO_HOST_RING_FRAMES
#define USB_AUDIO_HOST_RING_FRAMES 1024
#endif

// Largest isochronous packet the engine will request or send, in bytes. A
// high-speed endpoint may announce up to 1024 per transaction (3072 with
// additional transactions); an alternate setting whose packets exceed this is
// not selected.
#ifndef USB_AUDIO_HOST_PACKET_BYTES
#define USB_AUDIO_HOST_PACKET_BYTES 1024
#endif

// Frames (milliseconds) of transfer descriptors kept queued ahead of the host
// controller per stream. Also the longest interrupt latency the streams
// tolerate without a dropout. Must be at most a quarter of the periodic list
// length USBHost_t36 was built with (32 frames by default).
#ifndef USB_AUDIO_HOST_QUEUE_FRAMES
#define USB_AUDIO_HOST_QUEUE_FRAMES 4
#endif

// Highest number of discrete sample rates remembered from a UAC1 format
// descriptor or a UAC2 RANGE reply.
#ifndef USB_AUDIO_HOST_MAX_RATES
#define USB_AUDIO_HOST_MAX_RATES 12
#endif

// Alternate settings (formats) remembered per audio function, across all of
// its streaming interfaces.
#ifndef USB_AUDIO_HOST_MAX_FORMATS
#define USB_AUDIO_HOST_MAX_FORMATS 12
#endif

// Terminals and clock entities remembered from the control interface.
#ifndef USB_AUDIO_HOST_MAX_ENTITIES
#define USB_AUDIO_HOST_MAX_ENTITIES 16
#endif

#endif
