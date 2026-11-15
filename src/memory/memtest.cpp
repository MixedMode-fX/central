#include "memory/memtest.h"

namespace memtest {

uint32_t pattern(size_t index){
    // Knuth's multiplicative hash: neighbouring words differ in about half
    // their bits, and no two indices in a 32-bit space share a value.
    return (uint32_t)index * 2654435761u ^ 0xA5A5A5A5u;
}

size_t run(volatile uint32_t* base, size_t words, Settle settle){
    for (size_t i = 0; i < words; i++) base[i] = pattern(i);
    settle(base, words);
    size_t first = words;
    for (size_t i = 0; i < words; i++){
        if (base[i] != pattern(i) && first == words) first = i;
        base[i] = ~pattern(i);
    }
    settle(base, words);
    for (size_t i = 0; i < words && i < first; i++){
        if (base[i] != ~pattern(i)) return i;
    }
    return first;
}

}
