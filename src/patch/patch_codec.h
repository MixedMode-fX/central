#ifndef MMMC_PATCH_PATCH_CODEC_H
#define MMMC_PATCH_PATCH_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "node/patch.h"
#include "midi/scale.h"

// One serialisation of a Patch, used by EEPROM storage (#7) and by the SysEx
// protocol (#11). Written once so the two cannot drift, and so a patch that
// round-trips through the store round-trips over the wire byte for byte.
//
// **Why not store the struct raw.** `sizeof(Patch)` is about 11 KB: N_PARAM
// is 336 because PolySequencer and DrumSeqMidi carry a 32-step grid
// (config.h), and 32 nodes of that is far past the Teensy 4.1's ~4 KB of
// emulated EEPROM. Almost all of it is zero - every algorithm uses the first
// few parameter bytes and leaves the rest alone - so the encoder writes
// `n_nodes` nodes and, per node, only the parameter bytes up to the last
// non-zero one. A patch of logic and dividers is a couple of hundred bytes;
// one carrying a full poly sequencer is a few hundred more.
//
// That is not a translated representation: the bytes are NodeConfig's own
// bytes, in order, with a length in front. Decoding writes them straight back
// and zero-fills the tail, which is what the constructor would have seen.
//
// Everything here is 8-bit and allocation-free. #11 does the 7-bit packing
// the MIDI wire needs on top; the store writes these bytes as they are.

// Bumped when the layout below changes in a way an older decoder would
// misread. A decoder refuses a version it does not know rather than reading
// garbage into a live patch.
#define PATCH_FORMAT_VERSION 2

// "MMMC", big-endian, at the head of every stored or transmitted image.
#define PATCH_MAGIC 0x4D4D4D43u

enum CodecError : uint8_t {
    CODEC_OK = 0,
    CODEC_TRUNCATED,          // the buffer ended mid-record
    CODEC_BAD_MAGIC,
    CODEC_BAD_VERSION,
    CODEC_BAD_CRC,
    CODEC_TOO_MANY_NODES,     // n_nodes > N_NODE
    CODEC_PARAM_TOO_LONG,     // a node's parameter block exceeds N_PARAM
    CODEC_NO_ROOM,            // encoding did not fit the caller's buffer
    CODEC_TOO_MANY_MAPPINGS,  // more CC bindings than N_CC_MAP
};

// Global settings that travel with a patch: everything the module needs to
// come up the same way twice that is not a node (#7). The CV calibration #8
// adds goes in the reserved bytes rather than at the end, so the format
// version does not have to move for it.
struct GlobalSettings {
    uint8_t clock_source;      // MasterClock::Source
    uint8_t cv_ppqn;
    uint16_t bpm;
    uint8_t pc_enabled;        // Program Change recall on / off (#11)
    uint8_t pc_channel;        // 1..16, 0 = omni
    uint8_t pc_source_mask;    // MidiPort bits the recall listens on
    uint8_t pc_quantise;       // PatchSwapTiming (#11)
    // NRPN (#22). Off by default and enabled per port and channel, because
    // NRPN is a routable CC stream: 99/98/6/38 look like ordinary CCs to
    // everything upstream, so a module that always consumed them would
    // silently eat traffic meant for a downstream synth.
    uint8_t nrpn_enabled;
    uint8_t nrpn_channel;      // 1..16, 0 = omni
    uint8_t nrpn_source_mask;  // MidiPort bits; 0 = any
    // The key the module is in (midi/global_scale.h). Every algorithm with a
    // scale follows this one unless it names its own, so it is patch state
    // and not a node's: two of the reserved bytes rather than a new field at
    // the end, so the format version does not have to move for it.
    uint8_t scale;             // ScaleId; SCALE_GLOBAL / 0 reads as chromatic
    uint8_t root;              // pitch class, 0..11
    uint8_t reserved[19];      // #8's calibration lands here
};

inline GlobalSettings default_globals(){
    GlobalSettings g = {};
    g.clock_source = 0;                 // CLOCK_INTERNAL
    g.cv_ppqn = 4;
    g.bpm = CLOCK_DEFAULT_BPM;
    g.pc_enabled = 0;                   // off until a user asks for it (#11)
    g.pc_channel = 1;
    g.pc_source_mask = 0;
    g.pc_quantise = 0;
    g.nrpn_enabled = 0;                 // off until a user asks for it (#22)
    g.nrpn_channel = 0;
    g.nrpn_source_mask = 0;
    g.scale = SCALE_CHROMATIC;          // no key until a user sets one
    g.root = 0;
    return g;
}

namespace patch_codec {
    // The largest an encoded patch can be: the fixed header and port block,
    // plus a full parameter block for every node. Nothing ever reaches it in
    // practice; it is what a buffer must be able to hold to be safe.
    size_t max_encoded_size();

    // Encodes `patch` and `globals` into `out`, returning the number of bytes
    // written in `written`. The image carries the magic, the format version, a
    // length and a CRC-16 over everything before it, so a truncated or
    // corrupted image is detected rather than run.
    CodecError encode(const Patch& patch, const GlobalSettings& globals,
                      uint8_t* out, size_t capacity, size_t& written);

    // Decodes into `patch` and `globals`. On any error neither is touched
    // beyond what has already been written, so a caller decodes into a
    // staging buffer and only swaps on CODEC_OK (#11).
    CodecError decode(const uint8_t* in, size_t length,
                      Patch& patch, GlobalSettings& globals);

    // The CRC the image format uses: CRC-16/CCITT-FALSE. Small, adequate for
    // a few hundred bytes, and no table.
    uint16_t crc16(const uint8_t* data, size_t length);
}

#endif
