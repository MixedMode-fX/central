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
//   gate ports         GPIO_N x {direction 1, buses 2}
//   midi in            N_MIDI_IN_NODES x {source_mask 1, channel 1, buses 2}
//   midi out           N_MIDI_OUT_NODES x {target_mask 1, channel 1, buses 2}
//   n_nodes            1
//   nodes              n_nodes x {
//                          algorithm_id      1
//                          in length         1   (trailing empty sets trimmed)
//                          in_buses          2 x that many (a bit per bus)
//                          out length        1
//                          out_buses         2 x that many
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
//   n_routes           1   (modulation; unused slots are not stored)
//   routes             n_routes x {
//                          slot              1   which mod_map entry it is
//                          buses             2   CV buses, a bit per bus
//                          target_kind       1
//                          target_index      1
//                          param             2
//                          min               2
//                          max               2
//                          depth             1
//                          flags             1
//                      }
//   n_macros           1   (named macros; unnamed slots are not stored)
//   macros             n_macros x {
//                          slot              1   which macros[] entry it is
//                          name              MACRO_NAME_BYTES, space padded,
//                                                not NUL-terminated when full
//                      }
//   n_macro_dest       1   (the shared destination pool; unused not stored)
//   macro_dest         n_macro_dest x {
//                          slot              1   which macro_dest[] entry it is
//                          macro             1
//                          target_kind       1
//                          target_index      1
//                          param             2
//                          src_lo            1
//                          src_hi            1
//                          depth             2   signed, two's complement
//                          flags             1
//                      }

static constexpr size_t HEADER_BYTES = 8;
static constexpr size_t CRC_BYTES = 2;
static constexpr size_t PORT_BYTES =
    (size_t)GPIO_N * 3u + (size_t)N_MIDI_IN_NODES * 4u + (size_t)N_MIDI_OUT_NODES * 4u + 1u;
static constexpr size_t NODE_FIXED_BYTES = 1u + 1u + 2u * MAX_IN + 1u + 2u * MAX_OUT + 2u;
static constexpr size_t MAPPING_BYTES = 13u;
static constexpr size_t ROUTE_BYTES = 13u;
static constexpr size_t MACRO_BYTES = 1u + MACRO_NAME_BYTES;
static constexpr size_t MACRO_DEST_BYTES = 11u;

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
         + 1u + (size_t)N_CC_MAP * MAPPING_BYTES
         + 1u + (size_t)N_MOD_ROUTE * ROUTE_BYTES
         + 1u + (size_t)N_MACRO * MACRO_BYTES
         + 1u + (size_t)N_MACRO_DEST * MACRO_DEST_BYTES + CRC_BYTES;
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
        // A port's set of buses, a bit per bus.
        void set(BusSet s){ u16(s.bits); }
        void sets(const BusSet* src, size_t n){ for (size_t i = 0; i < n; i++) set(src[i]); }
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
        BusSet set(){ return BusSet{u16()}; }
        void sets(BusSet* dst, size_t n){ for (size_t i = 0; i < n; i++) dst[i] = set(); }
    };

    // The last non-zero parameter, plus one. An all-zero block encodes as
    // nothing at all, which is the common case for a logic gate.
    uint16_t used_params(const NodeConfig& node){
        uint16_t n = N_PARAM;
        while (n > 0 && node.params[n - 1] == 0) n--;
        return n;
    }

    // The last connected port, plus one - the same trimming the parameters
    // get, and for the same reason: most algorithms use one or two inlets and
    // one outlet, and a fixed MAX_IN + MAX_OUT of them is most of a node
    // record spent saying "nothing".
    uint8_t used_ports(const BusSet* ports, uint8_t n){
        while (n > 0 && !ports[n - 1].any()) n--;
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
        w.set(patch.gate_ports[i].buses);
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        w.u8(patch.midi_in[i].source_mask);
        w.u8(patch.midi_in[i].channel);
        w.set(patch.midi_in[i].buses);
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        w.u8(patch.midi_out[i].target_mask);
        w.u8(patch.midi_out[i].channel);
        w.set(patch.midi_out[i].buses);
    }

    w.u8(patch.n_nodes);
    for (uint8_t n = 0; n < patch.n_nodes; n++){
        const NodeConfig& node = patch.nodes[n];
        w.u8(node.algorithm_id);
        const uint8_t n_in = used_ports(node.in_buses, MAX_IN);
        w.u8(n_in);
        w.sets(node.in_buses, n_in);
        const uint8_t n_out = used_ports(node.out_buses, MAX_OUT);
        w.u8(n_out);
        w.sets(node.out_buses, n_out);
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

    // Modulation routes. Same shape as the bindings above and for the same
    // reason: only the slots in use are stored, so a patch with no modulation
    // costs one byte.
    uint8_t n_routes = 0;
    for (uint8_t i = 0; i < N_MOD_ROUTE; i++) if (patch.mod_map[i].buses.any()) n_routes++;
    w.u8(n_routes);
    for (uint8_t i = 0; i < N_MOD_ROUTE; i++){
        const ModRoute& r = patch.mod_map[i];
        if (!r.buses.any()) continue;
        w.u8(i);
        w.set(r.buses);
        w.u8(r.target_kind);
        w.u8(r.target_index);
        w.u16(r.param);
        w.u16(r.min);
        w.u16(r.max);
        w.u8(r.depth);
        w.u8(r.flags);
    }

    // Macros. A macro exists because it has a name - an unnamed slot holds
    // nothing a player could be told about - so the name is what decides
    // whether it is stored. Fixed width and not length-prefixed: this reader
    // is mirrored by hand in app/src/protocol/codec.js, and a fixed field is
    // the one shape that cannot disagree between the two.
    uint8_t n_macros = 0;
    for (uint8_t i = 0; i < N_MACRO; i++) if (macro_used(patch.macros[i])) n_macros++;
    w.u8(n_macros);
    for (uint8_t i = 0; i < N_MACRO; i++){
        const MacroDef& m = patch.macros[i];
        if (!macro_used(m)) continue;
        w.u8(i);
        w.bytes((const uint8_t*)m.name, MACRO_NAME_BYTES);
    }

    // The destination pool, shared across every macro.
    uint8_t n_dest = 0;
    for (uint8_t i = 0; i < N_MACRO_DEST; i++) if (patch.macro_dest[i].macro != MACRO_NONE) n_dest++;
    w.u8(n_dest);
    for (uint8_t i = 0; i < N_MACRO_DEST; i++){
        const MacroDest& d = patch.macro_dest[i];
        if (d.macro == MACRO_NONE) continue;
        w.u8(i);
        w.u8(d.macro);
        w.u8(d.target_kind);
        w.u8(d.target_index);
        w.u16(d.param);
        w.u8(d.src_lo);
        w.u8(d.src_hi);
        w.u16((uint16_t)d.depth);   // two's complement; signed on the way back
        w.u8(d.flags);
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
        patch.gate_ports[i].buses = r.set();
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        patch.midi_in[i].source_mask = r.u8();
        patch.midi_in[i].channel = r.u8();
        patch.midi_in[i].buses = r.set();
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        patch.midi_out[i].target_mask = r.u8();
        patch.midi_out[i].channel = r.u8();
        patch.midi_out[i].buses = r.set();
    }

    const uint8_t n_nodes = r.u8();
    if (r.underflowed) return CODEC_TRUNCATED;
    if (n_nodes > N_NODE) return CODEC_TOO_MANY_NODES;

    for (uint8_t n = 0; n < n_nodes; n++){
        NodeConfig& node = patch.nodes[n];
        node = node_config(0);                  // every port on no bus, params zero
        node.algorithm_id = r.u8();
        const uint8_t n_in = r.u8();
        if (n_in > MAX_IN) return CODEC_TRUNCATED;
        r.sets(node.in_buses, n_in);
        const uint8_t n_out = r.u8();
        if (n_out > MAX_OUT) return CODEC_TRUNCATED;
        r.sets(node.out_buses, n_out);
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
    if (r.underflowed) return CODEC_TRUNCATED;

    const uint8_t n_routes = r.u8();
    if (r.underflowed) return CODEC_TRUNCATED;
    if (n_routes > N_MOD_ROUTE) return CODEC_TOO_MANY_ROUTES;
    for (uint8_t i = 0; i < n_routes; i++){
        const uint8_t slot = r.u8();
        ModRoute route = unused_route();
        route.buses = r.set();
        route.target_kind = r.u8();
        route.target_index = r.u8();
        route.param = r.u16();
        route.min = r.u16();
        route.max = r.u16();
        route.depth = r.u8();
        route.flags = r.u8();
        if (r.underflowed) return CODEC_TRUNCATED;
        if (slot >= N_MOD_ROUTE) return CODEC_TOO_MANY_ROUTES;
        patch.mod_map[slot] = route;
    }

    const uint8_t n_macros = r.u8();
    if (r.underflowed) return CODEC_TRUNCATED;
    if (n_macros > N_MACRO) return CODEC_TOO_MANY_MACROS;
    for (uint8_t i = 0; i < n_macros; i++){
        const uint8_t slot = r.u8();
        MacroDef m = unused_macro();
        r.bytes((uint8_t*)m.name, MACRO_NAME_BYTES);
        if (r.underflowed) return CODEC_TRUNCATED;
        if (slot >= N_MACRO) return CODEC_TOO_MANY_MACROS;
        patch.macros[slot] = m;
    }

    const uint8_t n_dest = r.u8();
    if (r.underflowed) return CODEC_TRUNCATED;
    if (n_dest > N_MACRO_DEST) return CODEC_TOO_MANY_MACRO_DEST;
    for (uint8_t i = 0; i < n_dest; i++){
        const uint8_t slot = r.u8();
        MacroDest d = unused_dest();
        d.macro = r.u8();
        d.target_kind = r.u8();
        d.target_index = r.u8();
        d.param = r.u16();
        d.src_lo = r.u8();
        d.src_hi = r.u8();
        d.depth = (int16_t)r.u16();
        d.flags = r.u8();
        if (r.underflowed) return CODEC_TRUNCATED;
        if (slot >= N_MACRO_DEST) return CODEC_TOO_MANY_MACRO_DEST;
        patch.macro_dest[slot] = d;
    }
    return r.underflowed ? CODEC_TRUNCATED : CODEC_OK;
}
