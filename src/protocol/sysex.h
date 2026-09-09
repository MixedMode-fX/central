#ifndef MMMC_PROTOCOL_SYSEX_H
#define MMMC_PROTOCOL_SYSEX_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

// The MMMC patch protocol on the wire (#11).
//
// With no encoder, no switches and no display, this and the console are the
// only ways to configure the module. It is not a convenience feature sitting
// next to a panel menu; everything downstream of the bus model is unreachable
// by a user until it exists.
//
// **The wire format is the patch format.** A bulk transfer carries exactly
// the bytes patch_codec produces for EEPROM storage, so the store, the
// editor and the console all write the same image through the same validated
// apply path. There is no second representation to drift.

// Manufacturer ID. 0x7D is reserved by the MIDI specification for
// non-commercial and educational use, which is what this is.
//
// **It must never ship in a product.** A real manufacturer ID comes from the
// MIDI Association and costs money; whether that is in scope is a decision
// for whoever ships hardware, and until it is made every message on this
// protocol says "development build" by its second byte.
#define SYSEX_MANUFACTURER 0x7D

// Bumped when a message's layout changes in a way an older host would
// misread. A host that does not know this version is told so and writes
// nothing, rather than writing garbage into a live patch.
#define SYSEX_PROTOCOL_VERSION 1

// Universal SysEx, for the standard identity request every editor uses to
// find a device among the host's ports.
#define SYSEX_UNIVERSAL_NON_REALTIME 0x7E
#define SYSEX_GENERAL_INFORMATION 0x06
#define SYSEX_IDENTITY_REQUEST 0x01
#define SYSEX_IDENTITY_REPLY 0x02

// This module's family and member, under SYSEX_MANUFACTURER.
#define SYSEX_DEVICE_FAMILY 0x01
#define SYSEX_DEVICE_MEMBER 0x01

// Any device answers 0x7F; a specific one answers only its own id. There is
// one id per module so two on the same bus can be told apart.
#define SYSEX_BROADCAST_DEVICE 0x7F
#define SYSEX_DEFAULT_DEVICE 0x00

// Commands. Host -> device below 0x40, device -> host at 0x40 and above, so
// a module that hears its own reply looped back never acts on it.
enum SysexCommand : uint8_t {
    // Host -> device
    SYSEX_HELLO            = 0x01,   // who are you?
    SYSEX_CAPS_REQUEST     = 0x02,   // how big is everything?
    SYSEX_ALGO_REQUEST     = 0x03,   // what algorithms do you have?
    SYSEX_PARAM_REQUEST    = 0x04,   // <algorithm id>: describe its parameters
    SYSEX_DUMP_REQUEST     = 0x05,   // send me the running patch
    SYSEX_PATCH_CHUNK_IN   = 0x06,   // one chunk of a patch, into staging
    SYSEX_PATCH_ABORT      = 0x07,   // forget the partial transfer
    SYSEX_SET_PARAM        = 0x10,
    SYSEX_GET_PARAM        = 0x11,
    SYSEX_SET_CONNECTION   = 0x12,   // one inlet or outlet of one node
    SYSEX_SET_GATE_PORT    = 0x13,
    SYSEX_SET_MIDI_PORT    = 0x14,
    SYSEX_SET_GLOBALS      = 0x15,
    SYSEX_SET_CC_MAP       = 0x16,   // one controller binding (#21)
    SYSEX_GET_CC_MAP       = 0x17,
    SYSEX_CC_LEARN         = 0x18,   // arm / cancel: the next CC binds
    SYSEX_SET_NRPN         = 0x19,   // enable NRPN, per port and channel (#22)
    SYSEX_SET_PATTERN      = 0x1A,   // a run of one node's parameter bytes (#22)
    SYSEX_GET_PATTERN      = 0x1B,
    SYSEX_GET_CONTROL      = 0x1C,   // read any target, by kind and index
    SYSEX_SLOT_SAVE        = 0x20,
    SYSEX_SLOT_LOAD        = 0x21,
    SYSEX_SLOT_ERASE       = 0x22,
    SYSEX_SLOT_LIST        = 0x23,
    SYSEX_RESTORE_DEFAULTS = 0x30,

    // Device -> host
    SYSEX_IDENTITY         = 0x41,
    SYSEX_CAPABILITIES     = 0x42,
    SYSEX_ALGORITHM        = 0x43,
    SYSEX_PARAM_DESC       = 0x44,
    SYSEX_PATCH_CHUNK_OUT  = 0x45,
    SYSEX_PARAM_VALUE      = 0x51,
    SYSEX_CC_MAP           = 0x52,
    SYSEX_PATTERN          = 0x53,
    SYSEX_CONTROL_VALUE    = 0x54,
    SYSEX_SLOTS            = 0x63,
    SYSEX_ACK              = 0x70,
    SYSEX_NAK              = 0x71,   // <SysexError>
    SYSEX_EVENT            = 0x72,   // <SysexEvent> <detail>: the module speaking first
};

enum SysexError : uint8_t {
    SYSEX_ERR_NONE = 0,
    SYSEX_ERR_BAD_VERSION,
    SYSEX_ERR_TRUNCATED,        // the message ended mid-field
    SYSEX_ERR_UNKNOWN_COMMAND,
    SYSEX_ERR_BAD_ARGUMENT,     // an index or value outside what exists
    SYSEX_ERR_BAD_CHUNK,        // sequence number or checksum did not match
    SYSEX_ERR_TOO_LARGE,        // the transfer exceeds the staging buffer
    SYSEX_ERR_BAD_IMAGE,        // magic, version or CRC of the patch image
    SYSEX_ERR_REJECTED,         // the validator refused the patch
    SYSEX_ERR_NO_TRANSFER,      // a chunk arrived with no transfer running
    SYSEX_ERR_SLOT_EMPTY,
    SYSEX_ERR_SLOT_CORRUPT,
    SYSEX_ERR_BUSY,             // a swap is already pending
};

// Things the module originates, so an editor follows along without polling.
// With no panel there is no user-initiated drift to reconcile - these are the
// only state changes a host cannot have caused itself.
enum SysexEvent : uint8_t {
    SYSEX_EVENT_PROGRAM_CHANGE = 0x01,   // <slot>: a Program Change recalled a preset
    SYSEX_EVENT_PATCH_APPLIED  = 0x02,   // <0>: a pending quantised swap went live
    SYSEX_EVENT_ERROR          = 0x03,   // <SysexError>: what the red LED is reporting
    SYSEX_EVENT_CC_LEARNED     = 0x04,   // <slot>: a learn captured a controller
};

// Chunking. A DIN dump at 31250 baud is slow and the USB path has its own
// ceiling, so a patch is chunked whatever the library's SysExMaxSize turns
// out to be - and MidiSettings (src/mm_midi.h) does not raise it.
//
// SYSEX_CHUNK_PAYLOAD is the *unpacked* bytes per chunk. After 7-in-8 packing
// that is 8/7 as many wire bytes, plus the header and the F0/F7, which keeps
// a chunk comfortably inside any plausible receive buffer.
#define SYSEX_CHUNK_PAYLOAD 112
// Chunk header flags.
#define SYSEX_CHUNK_FIRST 0x01
#define SYSEX_CHUNK_LAST  0x02

// A partial transfer older than this is abandoned, and the active patch is
// never touched by one. Ten seconds is far longer than a full dump over DIN
// and short enough that a host which died mid-transfer does not leave the
// module waiting.
#define SYSEX_TRANSFER_TIMEOUT_US 10000000u

// The largest message the module will assemble. A chunk is bounded above; a
// received message longer than this is dropped rather than truncated, because
// a truncated command is a command with the wrong arguments.
#define SYSEX_RX_MAX 320
// The largest reply. An algorithm descriptor with its parameter names is the
// biggest thing sent in one message.
#define SYSEX_TX_MAX 320

namespace sysex {
    // 7-in-8 packing, the standard MIDI scheme: for every group of up to
    // seven bytes, one byte carrying their high bits followed by the seven
    // low-7-bit values. Every byte on the wire is therefore <= 0x7F, which is
    // what SysEx requires, and nothing in the payload can be mistaken for
    // F7.
    //
    // Returns the number of bytes written, or 0 if `capacity` is too small.
    size_t pack(const uint8_t* in, size_t length, uint8_t* out, size_t capacity);
    // The inverse. Returns the number of unpacked bytes, or 0 on a malformed
    // group or too small a buffer.
    size_t unpack(const uint8_t* in, size_t length, uint8_t* out, size_t capacity);
    // What pack() will produce for `length` bytes.
    size_t packed_size(size_t length);

    // A 7-bit XOR checksum over a chunk's packed bytes, so a chunk corrupted
    // in flight is caught before it reaches the staging buffer rather than at
    // the end of a whole transfer.
    uint8_t checksum(const uint8_t* data, size_t length);
}

#endif
