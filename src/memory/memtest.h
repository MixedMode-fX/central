#ifndef MMMC_MEMORY_MEMTEST_H
#define MMMC_MEMORY_MEMTEST_H

#include <stdint.h>
#include <stddef.h>

namespace memtest {
    // Pushes what the cache holds for a range out to the memory behind it and
    // forgets it, so the next read comes from the chip rather than the cache.
    // A test that skipped this would only be testing the cache.
    using Settle = void (*)(volatile uint32_t* base, size_t words);

    // Two passes over `words` words at `base`. The first writes a value
    // hashed from each address, so two addresses that alias the same cell -
    // a dead address line, a chip half the size it claims - read back each
    // other's value. The second writes the complement while checking, so
    // every bit of every cell has held both a 0 and a 1.
    //
    // Returns the index of the first word that did not hold, or `words` when
    // all of them did. Destroys whatever was there.
    size_t run(volatile uint32_t* base, size_t words, Settle settle);

    // The value pass one writes at word `index`.
    uint32_t pattern(size_t index);
}

#endif
