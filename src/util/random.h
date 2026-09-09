#ifndef MMMC_UTIL_RANDOM_H
#define MMMC_UTIL_RANDOM_H

#include <stdint.h>

// A tiny deterministic PRNG, and one place to get a seed from.
//
// Deterministic per instance: a RandomSequencer must play the same pattern
// until it is shredded, which rules out drawing from a shared stream. Seeded
// from entropy: it must not play the *same* pattern on every power cycle,
// which rules out a constant.
class Xorshift32 {
    public:
        explicit Xorshift32(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}

        uint32_t next(){
            uint32_t x = state;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            state = x;
            return x;
        }
        // 0 .. limit-1, without the modulo bias that matters at small limits.
        uint8_t below(uint8_t limit){
            if (limit <= 1) return 0;
            const uint32_t span = 0xFFFFFFFFu / limit;
            const uint32_t ceiling = span * limit;
            uint32_t v = next();
            while (v >= ceiling) v = next();
            return (uint8_t)(v / span);
        }
        // True `percent` times in a hundred.
        bool chance(uint8_t percent){
            if (percent == 0) return false;
            if (percent >= 100) return true;
            return below(100) < percent;
        }
        void reseed(uint32_t seed){ state = seed ? seed : 0x9E3779B9u; }

    private:
        uint32_t state;
};

// The module's entropy pool. main.cpp stirs it at boot from something that
// differs between power cycles (the cycle counter, a floating ADC pin); the
// native tests set it explicitly, which is what makes "a different pattern
// after a power cycle" a test rather than a hope.
namespace entropy {
    // Mixed into the pool rather than replacing it, so several weak sources
    // can be added without one of them cancelling the others.
    inline uint32_t& pool(){
        static uint32_t value = 0x12345678u;
        return value;
    }
    inline void stir(uint32_t value){
        uint32_t p = pool() ^ value;
        p ^= p << 13; p ^= p >> 17; p ^= p << 5;
        pool() = p;
    }
    // A fresh seed for a node. Advances the pool, so two nodes constructed in
    // the same patch do not share a stream.
    inline uint32_t seed(){
        stir(0xA5A5A5A5u);
        return pool();
    }
}

#endif
