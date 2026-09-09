#include "protocol/sysex.h"

size_t sysex::packed_size(size_t length){
    const size_t groups = (length + 6u) / 7u;
    return length + groups;
}

size_t sysex::pack(const uint8_t* in, size_t length, uint8_t* out, size_t capacity){
    if (packed_size(length) > capacity) return 0;
    size_t at = 0;
    for (size_t i = 0; i < length; i += 7){
        const size_t n = (length - i) < 7u ? (length - i) : 7u;
        uint8_t high = 0;
        for (size_t b = 0; b < n; b++){
            if (in[i + b] & 0x80u) high = (uint8_t)(high | (1u << b));
        }
        out[at++] = high;
        for (size_t b = 0; b < n; b++) out[at++] = (uint8_t)(in[i + b] & 0x7Fu);
    }
    return at;
}

size_t sysex::unpack(const uint8_t* in, size_t length, uint8_t* out, size_t capacity){
    size_t at = 0;
    size_t i = 0;
    while (i < length){
        const uint8_t high = in[i++];
        if (high & 0x80u) return 0;                  // not a 7-bit stream
        size_t n = length - i;
        if (n > 7u) n = 7u;
        if (n == 0) break;                           // a trailing high byte with no data
        if (at + n > capacity) return 0;
        for (size_t b = 0; b < n; b++){
            const uint8_t value = in[i + b];
            if (value & 0x80u) return 0;
            out[at++] = (uint8_t)(value | ((high & (1u << b)) ? 0x80u : 0u));
        }
        i += n;
    }
    return at;
}

uint8_t sysex::checksum(const uint8_t* data, size_t length){
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) sum = (uint8_t)(sum ^ data[i]);
    return (uint8_t)(sum & 0x7Fu);
}
