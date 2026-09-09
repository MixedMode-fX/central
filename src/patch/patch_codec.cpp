#include "patch/patch_codec.h"

// Image layout, all little-endian where a field is wider than a byte:
//
//   0   magic          4   "MMMC"
//   4   version        1   PATCH_FORMAT_VERSION
//   5   flags          1   reserved, zero
//   6   payload length 2   bytes of payload that follow the header
//   8   payload      ...
//   8+n crc16          2   over bytes 0 .. 8+n-1
//
// Payload:
//   globals            sizeof(GlobalSettings), as bytes
//   gate ports         GPIO_N x {direction, bus}
//   midi in            N_MIDI_IN_NODES x {source_mask, channel, bus}
//   midi out           N_MIDI_OUT_NODES x {target_mask, channel, bus}
//   n_nodes            1
//   nodes              n_nodes x {
//                          algorithm_id      1
//                          in_bus            MAX_IN
//                          out_bus           MAX_OUT
//                          param length      2   (trailing zeros trimmed)
//                          params            that many bytes
//                      }
//   n_mappings         1   (#21; unused slots are not stored)
//   mappings           n_mappings x {
//                          slot              1   which cc_map entry it is
//                          source_mask       1
//                          channel           1
//                          cc                1
//                          target_kind       1
//                          target_index      1
//                          param             2
//                          min               2
//                          max               2
//                          flags             1
//                      }

static constexpr size_t HEADER_BYTES = 8;
static constexpr size_t CRC_BYTES = 2;
static constexpr size_t PORT_BYTES =
    (size_t)GPIO_N * 2u + (size_t)N_MIDI_IN_NODES * 3u + (size_t)N_MIDI_OUT_NODES * 3u + 1u;
static constexpr size_t NODE_FIXED_BYTES = 1u + MAX_IN + MAX_OUT + 2u;
static constexpr size_t MAPPING_BYTES = 13u;

uint16_t patch_codec::crc16(const uint8_t* data, size_t length){
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++){
        crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
        for (uint8_t bit = 0; bit < 8; bit++){
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t patch_codec::max_encoded_size(){
    return HEADER_BYTES + sizeof(GlobalSettings) + PORT_BYTES
         + (size_t)N_NODE * (NODE_FIXED_BYTES + N_PARAM)
         + 1u + (size_t)N_CC_MAP * MAPPING_BYTES + CRC_BYTES;
}

// A small append-only writer, so every put is bounds-checked in one place.
namespace {
    struct Writer {
        uint8_t* out;
        size_t capacity;
        size_t at;
        bool overflowed;

        void u8(uint8_t v){
            if (at >= capacity){ overflowed = true; return; }
            out[at++] = v;
        }
        void u16(uint16_t v){ u8((uint8_t)(v & 0xFFu)); u8((uint8_t)(v >> 8)); }
        void u32(uint32_t v){ u16((uint16_t)(v & 0xFFFFu)); u16((uint16_t)(v >> 16)); }
        void bytes(const uint8_t* src, size_t n){ for (size_t i = 0; i < n; i++) u8(src[i]); }
    };

    struct Reader {
        const uint8_t* in;
        size_t length;
        size_t at;
        bool underflowed;

        uint8_t u8(){
            if (at >= length){ underflowed = true; return 0; }
            return in[at++];
        }
        uint16_t u16(){ const uint16_t lo = u8(); return (uint16_t)(lo | ((uint16_t)u8() << 8)); }
        uint32_t u32(){ const uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
        void bytes(uint8_t* dst, size_t n){ for (size_t i = 0; i < n; i++) dst[i] = u8(); }
    };

    // The last non-zero parameter, plus one. An all-zero block encodes as
    // nothing at all, which is the common case for a logic gate.
    uint16_t used_params(const NodeConfig& node){
        uint16_t n = N_PARAM;
        while (n > 0 && node.params[n - 1] == 0) n--;
        return n;
    }
}

CodecError patch_codec::encode(const Patch& patch, const GlobalSettings& globals,
                               uint8_t* out, size_t capacity, size_t& written){
    written = 0;
    if (patch.n_nodes > N_NODE) return CODEC_TOO_MANY_NODES;

    Writer w{out, capacity, 0, false};
    w.u32(PATCH_MAGIC);
    w.u8(PATCH_FORMAT_VERSION);
    w.u8(0);                                   // flags
    w.u16(0);                                  // payload length, filled in below
    const size_t payload_start = w.at;

    w.bytes(reinterpret_cast<const uint8_t*>(&globals), sizeof(GlobalSettings));

    for (uint8_t i = 0; i < GPIO_N; i++){
        w.u8(patch.gate_ports[i].direction);
        w.u8(patch.gate_ports[i].bus);
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        w.u8(patch.midi_in[i].source_mask);
        w.u8(patch.midi_in[i].channel);
        w.u8(patch.midi_in[i].bus);
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        w.u8(patch.midi_out[i].target_mask);
        w.u8(patch.midi_out[i].channel);
        w.u8(patch.midi_out[i].bus);
    }

    w.u8(patch.n_nodes);
    for (uint8_t n = 0; n < patch.n_nodes; n++){
        const NodeConfig& node = patch.nodes[n];
        w.u8(node.algorithm_id);
        w.bytes(node.in_bus, MAX_IN);
        w.bytes(node.out_bus, MAX_OUT);
        const uint16_t n_params = used_params(node);
        w.u16(n_params);
        w.bytes(node.params, n_params);
    }

    // Controller bindings (#21). Only the slots in use are stored, so a patch
    // with no mappings costs one byte.
    uint8_t n_mappings = 0;
    for (uint8_t i = 0; i < N_CC_MAP; i++) if (patch.cc_map[i].source_mask != 0) n_mappings++;
    w.u8(n_mappings);
    for (uint8_t i = 0; i < N_CC_MAP; i++){
        const CcMapping& m = patch.cc_map[i];
        if (m.source_mask == 0) continue;
        w.u8(i);
        w.u8(m.source_mask);
        w.u8(m.channel);
        w.u8(m.cc);
        w.u8(m.target_kind);
        w.u8(m.target_index);
        w.u16(m.param);
        w.u16(m.min);
        w.u16(m.max);
        w.u8(m.flags);
    }

    if (w.overflowed) return CODEC_NO_ROOM;

    const size_t payload = w.at - payload_start;
    out[6] = (uint8_t)(payload & 0xFFu);
    out[7] = (uint8_t)(payload >> 8);

    const uint16_t crc = crc16(out, w.at);
    w.u16(crc);
    if (w.overflowed) return CODEC_NO_ROOM;

    written = w.at;
    return CODEC_OK;
}

CodecError patch_codec::decode(const uint8_t* in, size_t length,
                               Patch& patch, GlobalSettings& globals){
    if (length < HEADER_BYTES + CRC_BYTES) return CODEC_TRUNCATED;

    Reader r{in, length, 0, false};
    if (r.u32() != PATCH_MAGIC) return CODEC_BAD_MAGIC;
    if (r.u8() != PATCH_FORMAT_VERSION) return CODEC_BAD_VERSION;
    r.u8();                                     // flags
    const uint16_t payload = r.u16();
    if (HEADER_BYTES + (size_t)payload + CRC_BYTES > length) return CODEC_TRUNCATED;

    // The CRC covers the header and the payload; check it before anything is
    // interpreted, so a corrupted image never reaches the validator.
    const size_t crc_at = HEADER_BYTES + payload;
    const uint16_t stored = (uint16_t)(in[crc_at] | ((uint16_t)in[crc_at + 1] << 8));
    if (stored != crc16(in, crc_at)) return CODEC_BAD_CRC;

    r.length = crc_at;                          // nothing past the payload is readable

    patch = empty_patch();
    r.bytes(reinterpret_cast<uint8_t*>(&globals), sizeof(GlobalSettings));

    for (uint8_t i = 0; i < GPIO_N; i++){
        patch.gate_ports[i].direction = r.u8();
        patch.gate_ports[i].bus = r.u8();
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        patch.midi_in[i].source_mask = r.u8();
        patch.midi_in[i].channel = r.u8();
        patch.midi_in[i].bus = r.u8();
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        patch.midi_out[i].target_mask = r.u8();
        patch.midi_out[i].channel = r.u8();
        patch.midi_out[i].bus = r.u8();
    }

    const uint8_t n_nodes = r.u8();
    if (r.underflowed) return CODEC_TRUNCATED;
    if (n_nodes > N_NODE) return CODEC_TOO_MANY_NODES;

    for (uint8_t n = 0; n < n_nodes; n++){
        NodeConfig& node = patch.nodes[n];
        node = node_config(0);                  // NO_BUS everywhere, params zero
        node.algorithm_id = r.u8();
        r.bytes(node.in_bus, MAX_IN);
        r.bytes(node.out_bus, MAX_OUT);
        const uint16_t n_params = r.u16();
        if (r.underflowed) return CODEC_TRUNCATED;
        if (n_params > N_PARAM) return CODEC_PARAM_TOO_LONG;
        r.bytes(node.params, n_params);         // the tail stays zero
        if (r.underflowed) return CODEC_TRUNCATED;
    }
    patch.n_nodes = n_nodes;

    const uint8_t n_mappings = r.u8();
    if (r.underflowed) return CODEC_TRUNCATED;
    if (n_mappings > N_CC_MAP) return CODEC_TOO_MANY_MAPPINGS;
    for (uint8_t i = 0; i < n_mappings; i++){
        const uint8_t slot = r.u8();
        CcMapping m = unused_mapping();
        m.source_mask = r.u8();
        m.channel = r.u8();
        m.cc = r.u8();
        m.target_kind = r.u8();
        m.target_index = r.u8();
        m.param = r.u16();
        m.min = r.u16();
        m.max = r.u16();
        m.flags = r.u8();
        if (r.underflowed) return CODEC_TRUNCATED;
        if (slot >= N_CC_MAP) return CODEC_TOO_MANY_MAPPINGS;
        patch.cc_map[slot] = m;
    }
    return r.underflowed ? CODEC_TRUNCATED : CODEC_OK;
}
