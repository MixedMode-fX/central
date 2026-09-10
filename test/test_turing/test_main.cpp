#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "algorithm/modulator/turing.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// Turing (#32): the axis between a locked loop and pure noise, which is the
// one shape the module's random sources had no point on.

static const uint8_t GATE_ADVANCE = 0, GATE_RESET = 1, GATE_PULSE = 2;
static const uint8_t CV_OUT = 0;

// `seed` is never left at zero here: at zero the register is drawn from the
// entropy pool, which is right on a module and useless in a test.
static NodeConfig turing_config(uint8_t length, uint8_t chaos, uint8_t seed){
    NodeConfig c = node_config(ALGO_TURING);
    c.in_bus[0] = GATE_ADVANCE;
    c.in_bus[1] = GATE_RESET;
    c.out_bus[0] = GATE_PULSE;
    c.out_bus[1] = CV_OUT;
    c.params[0] = length;
    c.params[1] = chaos;
    c.params[4] = seed;
    return c;
}

// One advance edge, and what the pulse outlet did with it.
static bool advance(Turing& node, BusManager& bus, uint32_t& now_us){
    bus.gate_write(GATE_ADVANCE, false);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    now_us += 1000;

    bus.gate_write(GATE_ADVANCE, true);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    const bool fired = bus.gate_read(GATE_PULSE);
    now_us += 1000;
    return fired;
}

static void pulse_reset(Turing& node, BusManager& bus, uint32_t& now_us){
    bus.gate_write(GATE_RESET, true);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    now_us += 1000;
    bus.gate_write(GATE_RESET, false);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    now_us += 1000;
}

// ---------------------------------------------------------------------------
// The chaos axis: the three landmarks the control is built around.
// ---------------------------------------------------------------------------

static void test_chaos_zero_is_a_loop_that_repeats_for_ever() {
    BusManager bus;
    NodeConfig c = turing_config(8, 0, 77);
    Turing node(c);
    uint32_t now = 0;

    uint32_t first[8];
    for (uint8_t i = 0; i < 8; i++){ advance(node, bus, now); first[i] = node.pattern(); }

    // Sixteen more times round: every step is the step it was.
    for (uint8_t round = 0; round < 16; round++){
        for (uint8_t i = 0; i < 8; i++){
            advance(node, bus, now);
            TEST_ASSERT_EQUAL_UINT32(first[i], node.pattern());
        }
    }
}

static void test_chaos_one_hundred_is_the_loop_and_its_negative() {
    BusManager bus;
    NodeConfig c = turing_config(8, 100, 77);
    Turing node(c);
    uint32_t now = 0;

    uint32_t first[8];
    for (uint8_t i = 0; i < 8; i++){ advance(node, bus, now); first[i] = node.pattern(); }

    // One length on, every bit is the other way round ...
    const uint32_t mask = 0xFFu;
    for (uint8_t i = 0; i < 8; i++){
        advance(node, bus, now);
        TEST_ASSERT_EQUAL_UINT32(first[i] ^ mask, node.pattern());
    }
    // ... and one more length on it is back. A loop of twice the length.
    for (uint8_t i = 0; i < 8; i++){
        advance(node, bus, now);
        TEST_ASSERT_EQUAL_UINT32(first[i], node.pattern());
    }
}

static void test_chaos_is_the_probability_it_says_it_is() {
    BusManager bus;
    const uint8_t LEN = 8;
    // Deterministic: the seed is what makes this a test and not a hope.
    NodeConfig c = turing_config(LEN, 8, 42);
    Turing node(c);
    uint32_t now = 0;

    // The register is a rotation, so the bit fed in at step t is the bit fed
    // back at step t + length. Comparing the two is exactly one trial of the
    // chaos coin, which is what makes "the loop survives and slips" a number
    // rather than an impression.
    bool fed[LEN];
    for (uint8_t i = 0; i < LEN; i++){ advance(node, bus, now); fed[i] = node.current(); }

    uint32_t inverted = 0;
    const uint32_t rounds = 64;
    for (uint32_t r = 0; r < rounds; r++){
        for (uint8_t i = 0; i < LEN; i++){
            advance(node, bus, now);
            if (node.current() != fed[i]) inverted++;
            fed[i] = node.current();
        }
    }
    const uint32_t trials = rounds * LEN;                 // 512
    // 8% of 512 is 41, with a standard deviation of six. These bounds are
    // four deviations out either way: the loop is slipping, and it is
    // slipping at the rate the control asked for.
    TEST_ASSERT_TRUE_MESSAGE(inverted > trials / 50, "a slipping loop that never slipped");
    TEST_ASSERT_TRUE_MESSAGE(inverted < trials / 5, "chaos 8 behaved like chaos 50");
}

// The other end of the same measurement: at 50 the bit has no memory at all,
// so half the loop is gone every time round.
static void test_chaos_fifty_keeps_nothing() {
    BusManager bus;
    const uint8_t LEN = 8;
    NodeConfig c = turing_config(LEN, 50, 42);
    Turing node(c);
    uint32_t now = 0;

    bool fed[LEN];
    for (uint8_t i = 0; i < LEN; i++){ advance(node, bus, now); fed[i] = node.current(); }
    uint32_t inverted = 0;
    const uint32_t rounds = 64;
    for (uint32_t r = 0; r < rounds; r++){
        for (uint8_t i = 0; i < LEN; i++){
            advance(node, bus, now);
            if (node.current() != fed[i]) inverted++;
            fed[i] = node.current();
        }
    }
    const uint32_t trials = rounds * LEN;
    TEST_ASSERT_TRUE(inverted > trials * 2 / 5 && inverted < trials * 3 / 5);
}

static void test_a_locked_loop_visits_exactly_its_own_length_of_patterns() {
    BusManager bus;
    NodeConfig c = turing_config(6, 0, 13);
    Turing node(c);
    uint32_t now = 0;

    uint32_t seen[64];
    uint8_t n = 0;
    for (uint32_t i = 0; i < 200; i++){
        advance(node, bus, now);
        bool known = false;
        for (uint8_t k = 0; k < n; k++) if (seen[k] == node.pattern()) known = true;
        if (!known && n < 64) seen[n++] = node.pattern();
    }
    TEST_ASSERT_EQUAL_UINT8(6, n);
}

// ---------------------------------------------------------------------------
// The two outlets
// ---------------------------------------------------------------------------

static void test_the_pulse_is_the_bit_that_came_round() {
    BusManager bus;
    NodeConfig c = turing_config(8, 0, 77);
    c.params[5] = 1;                       // a 1 ms trigger, shorter than a step
    Turing node(c);
    uint32_t now = 0;

    for (uint32_t i = 0; i < 32; i++){
        const bool fired = advance(node, bus, now);
        TEST_ASSERT_EQUAL_MESSAGE(node.current(), fired, "pulse disagreed with the register");
    }
}

static void test_the_cv_and_the_pulse_come_from_one_register() {
    BusManager bus;
    // One bit read as CV: the level is the rails, and it is the same bit the
    // pulse plays. This is the property the node exists for - a rhythm and a
    // melody that move together.
    NodeConfig c = turing_config(8, 0, 77);
    c.params[2] = 1;
    c.params[5] = 1;
    Turing node(c);
    uint32_t now = 0;

    for (uint32_t i = 0; i < 32; i++){
        const bool fired = advance(node, bus, now);
        TEST_ASSERT_EQUAL_INT16(fired ? CV_MAX : 0, bus.cv_read(CV_OUT));
    }
}

static void test_bits_is_a_resolution_control_not_a_range_control() {
    BusManager bus;
    // However few bits the CV reads, an empty register is the bottom of the
    // bus and a full one is the top: `bits` decides how many levels there are
    // between them, never how far apart they are. One bit is both rails, not
    // the bottom half of the range.
    for (uint8_t bits = 1; bits <= Turing::MAX_BITS; bits++){
        NodeConfig c = turing_config(8, 0, 77);
        c.params[2] = bits;
        c.params[3] = Turing::TUR_CLEAR;
        Turing node(c);
        uint32_t now = 0;
        for (uint8_t i = 0; i < 8; i++) advance(node, bus, now);
        TEST_ASSERT_EQUAL_INT16_MESSAGE(0, bus.cv_read(CV_OUT), "an empty register was not the bottom");

        TEST_ASSERT_TRUE(node.set_param(3, Turing::TUR_FILL));
        for (uint8_t i = 0; i < 8; i++) advance(node, bus, now);
        TEST_ASSERT_EQUAL_INT16_MESSAGE(CV_MAX, bus.cv_read(CV_OUT), "a full register was not the top");
    }
}

// A reading finer than the ring has bits reads the bits that are not there as
// zero rather than as noise, so `bits` past `length` is harmless.
static void test_reading_more_bits_than_the_ring_has_is_harmless() {
    BusManager bus;
    NodeConfig c = turing_config(4, 0, 77);
    c.params[2] = 8;
    c.params[3] = Turing::TUR_FILL;
    Turing node(c);
    uint32_t now = 0;
    for (uint8_t i = 0; i < 4; i++) advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT32(0x0Fu, node.pattern());
    TEST_ASSERT_EQUAL_INT16(CV_MAX, bus.cv_read(CV_OUT));
}

static void test_a_bipolar_register_is_centred_on_zero() {
    BusManager bus;
    NodeConfig c = turing_config(8, 0, 77);
    c.params[6] = Turing::TUR_BIPOLAR;
    Turing node(c);
    uint32_t now = 0;
    int16_t low = CV_MAX, high = -CV_HALF;
    for (uint32_t i = 0; i < 64; i++){
        advance(node, bus, now);
        const int16_t v = bus.cv_read(CV_OUT);
        if (v < low) low = v;
        if (v > high) high = v;
    }
    TEST_ASSERT_TRUE(low < 0);
    TEST_ASSERT_TRUE(high > 0);
    TEST_ASSERT_TRUE(low >= -CV_HALF && high <= CV_HALF - 1);
}

// ---------------------------------------------------------------------------
// The hand on the register
// ---------------------------------------------------------------------------

static void test_reset_returns_to_the_trunk() {
    BusManager bus;
    NodeConfig c = turing_config(8, 50, 99);       // wandering freely
    Turing node(c);
    uint32_t now = 0;

    const uint32_t trunk = node.pattern();
    for (uint32_t i = 0; i < 200; i++) advance(node, bus, now);
    TEST_ASSERT_TRUE_MESSAGE(node.pattern() != trunk, "a chaos of 50 that never moved");

    pulse_reset(node, bus, now);
    TEST_ASSERT_EQUAL_UINT32(trunk, node.pattern());
}

static void test_clear_empties_the_loop_a_step_at_a_time() {
    BusManager bus;
    NodeConfig c = turing_config(8, 0, 77);
    Turing node(c);
    uint32_t now = 0;
    for (uint32_t i = 0; i < 8; i++) advance(node, bus, now);

    TEST_ASSERT_TRUE(node.set_param(3, Turing::TUR_CLEAR));
    for (uint8_t i = 0; i < 7; i++) advance(node, bus, now);
    TEST_ASSERT_TRUE_MESSAGE(node.pattern() != 0, "cleared in fewer steps than it has");
    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT32(0, node.pattern());

    // Fill puts every step back, and neither is a reset: the loop was
    // rewritten while it ran.
    TEST_ASSERT_TRUE(node.set_param(3, Turing::TUR_FILL));
    for (uint8_t i = 0; i < 8; i++) advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT32(0xFFu, node.pattern());
}

static void test_shortening_a_loop_keeps_the_steps_that_just_played() {
    BusManager bus;
    NodeConfig c = turing_config(8, 0, 77);
    Turing node(c);
    uint32_t now = 0;
    for (uint32_t i = 0; i < 8; i++) advance(node, bus, now);

    const uint32_t before = node.pattern();
    TEST_ASSERT_TRUE(node.set_param(0, 4));
    TEST_ASSERT_EQUAL_UINT32(before & 0x0Fu, node.pattern());
    // And it loops at the new length from there.
    uint32_t first[4];
    for (uint8_t i = 0; i < 4; i++){ advance(node, bus, now); first[i] = node.pattern(); }
    for (uint8_t i = 0; i < 4; i++){
        advance(node, bus, now);
        TEST_ASSERT_EQUAL_UINT32(first[i], node.pattern());
    }
}

static void test_a_new_seed_is_what_the_next_reset_returns_to() {
    BusManager bus;
    NodeConfig c = turing_config(8, 0, 77);
    Turing node(c);
    uint32_t now = 0;
    const uint32_t running = node.pattern();

    // Moving the seed must not shred the pattern under a running sequence -
    // a knob sweep would be unplayable. It changes where reset goes.
    TEST_ASSERT_TRUE(node.set_param(4, 123));
    TEST_ASSERT_EQUAL_UINT32(running, node.pattern());
    pulse_reset(node, bus, now);
    TEST_ASSERT_TRUE_MESSAGE(node.pattern() != running, "the new seed drew the old pattern");

    // And it is reproducible: the same seed is the same trunk.
    NodeConfig other = turing_config(8, 0, 123);
    Turing twin(other);
    TEST_ASSERT_EQUAL_UINT32(twin.pattern(), node.pattern());
}

// With no seed there is no trunk to return to, so the reset inlet is a
// shred - the same gesture RandomSequencer's third inlet is, and the reason
// `seed` is the parameter that decides which of the two the cable means.
static void test_reset_with_no_seed_shreds_instead() {
    BusManager bus;
    NodeConfig c = turing_config(16, 0, 0);
    Turing node(c);
    uint32_t now = 0;

    uint32_t seen[8];
    uint8_t distinct = 0;
    for (uint8_t i = 0; i < 8; i++){
        pulse_reset(node, bus, now);
        bool known = false;
        for (uint8_t k = 0; k < distinct; k++) if (seen[k] == node.pattern()) known = true;
        if (!known) seen[distinct++] = node.pattern();
    }
    TEST_ASSERT_TRUE_MESSAGE(distinct > 5, "an unseeded reset kept drawing the same pattern");
}

static void test_two_registers_on_one_seed_agree_and_on_none_do_not() {
    NodeConfig a = turing_config(16, 0, 55);
    NodeConfig b = turing_config(16, 0, 55);
    Turing one(a), two(b);
    TEST_ASSERT_EQUAL_UINT32(one.pattern(), two.pattern());

    // Seed 0 draws from the entropy pool, which advances per node, so two
    // registers in one patch do not play in unison by accident.
    NodeConfig c = turing_config(16, 0, 0);
    NodeConfig d = turing_config(16, 0, 0);
    Turing three(c), four(d);
    TEST_ASSERT_TRUE(three.pattern() != four.pattern());
}

static void test_the_register_never_allocates() {
    const size_t before = g_allocations;
    BusManager bus;
    NodeConfig c = turing_config(16, 25, 7);
    Turing node(c);
    uint32_t now = 0;
    for (uint32_t i = 0; i < 500; i++) advance(node, bus, now);
    TEST_ASSERT_EQUAL_size_t(before, g_allocations);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_chaos_zero_is_a_loop_that_repeats_for_ever);
    RUN_TEST(test_chaos_one_hundred_is_the_loop_and_its_negative);
    RUN_TEST(test_chaos_is_the_probability_it_says_it_is);
    RUN_TEST(test_chaos_fifty_keeps_nothing);
    RUN_TEST(test_a_locked_loop_visits_exactly_its_own_length_of_patterns);
    RUN_TEST(test_the_pulse_is_the_bit_that_came_round);
    RUN_TEST(test_the_cv_and_the_pulse_come_from_one_register);
    RUN_TEST(test_bits_is_a_resolution_control_not_a_range_control);
    RUN_TEST(test_reading_more_bits_than_the_ring_has_is_harmless);
    RUN_TEST(test_a_bipolar_register_is_centred_on_zero);
    RUN_TEST(test_reset_returns_to_the_trunk);
    RUN_TEST(test_clear_empties_the_loop_a_step_at_a_time);
    RUN_TEST(test_shortening_a_loop_keeps_the_steps_that_just_played);
    RUN_TEST(test_a_new_seed_is_what_the_next_reset_returns_to);
    RUN_TEST(test_reset_with_no_seed_shreds_instead);
    RUN_TEST(test_two_registers_on_one_seed_agree_and_on_none_do_not);
    RUN_TEST(test_the_register_never_allocates);
    return UNITY_END();
}
