#include <unity.h>
#include "memory/memtest.h"

void setUp() {}
void tearDown() {}

// The boot test of the PSRAM (hal/teensy/teensy_psram.cpp), on a buffer in
// host memory, with the settle hook standing in for the chip's faults: it
// runs exactly where the data cache would be flushed to the chip.

static constexpr size_t WORDS = 4096;
static uint32_t ram[WORDS];

static void good(volatile uint32_t*, size_t) {}

static size_t stuck_word = 0;
static uint32_t stuck_bit = 0;
static void stuck_low(volatile uint32_t* base, size_t) { base[stuck_word] &= ~stuck_bit; }
static void stuck_high(volatile uint32_t* base, size_t) { base[stuck_word] |= stuck_bit; }

// A chip half the size it says: the top half's cells are the bottom half's.
static void aliased(volatile uint32_t* base, size_t words) {
    for (size_t i = words / 2; i < words; i++) base[i - words / 2] = base[i];
}

static void test_good_memory_passes() {
    TEST_ASSERT_EQUAL(WORDS, memtest::run(ram, WORDS, good));
}

static void test_a_bit_stuck_low_is_found_where_it_is() {
    stuck_word = 1234;
    for (uint32_t bit = 0; bit < 32; bit++) {
        stuck_bit = 1u << bit;
        TEST_ASSERT_EQUAL(stuck_word, memtest::run(ram, WORDS, stuck_low));
    }
}

static void test_a_bit_stuck_high_is_found_where_it_is() {
    stuck_word = 77;
    for (uint32_t bit = 0; bit < 32; bit++) {
        stuck_bit = 1u << bit;
        TEST_ASSERT_EQUAL(stuck_word, memtest::run(ram, WORDS, stuck_high));
    }
}

static void test_aliased_addresses_are_found() {
    TEST_ASSERT_EQUAL(0, memtest::run(ram, WORDS, aliased));
}

static void test_neighbouring_patterns_differ() {
    for (size_t i = 0; i + 1 < WORDS; i++) {
        TEST_ASSERT_NOT_EQUAL(memtest::pattern(i), memtest::pattern(i + 1));
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_good_memory_passes);
    RUN_TEST(test_a_bit_stuck_low_is_found_where_it_is);
    RUN_TEST(test_a_bit_stuck_high_is_found_where_it_is);
    RUN_TEST(test_aliased_addresses_are_found);
    RUN_TEST(test_neighbouring_patterns_differ);
    return UNITY_END();
}
