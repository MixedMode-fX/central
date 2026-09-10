#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "algorithm/sequencer/automaton.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// Automaton (#34): the rhythm family was typed-in, a formula, or a coin
// flip, and nothing was in the middle.

static const uint8_t GATE_ADVANCE = 0, GATE_RESEED = 1, LANE_BASE = 2;

static NodeConfig automaton_config(uint8_t rule, uint8_t seed, uint8_t edges,
                                   uint8_t revive, uint8_t cells){
    NodeConfig c = node_config(ALGO_AUTOMATON);
    c.in_bus[0] = GATE_ADVANCE;
    c.in_bus[1] = GATE_RESEED;
    for (uint8_t i = 0; i < Automaton::LANES; i++) c.out_bus[i] = (uint8_t)(LANE_BASE + i);
    c.params[0] = rule;
    c.params[1] = seed;
    c.params[2] = edges;
    c.params[3] = revive;
    c.params[4] = cells;
    c.params[6] = 1;                // a 1 ms trigger, shorter than a step
    return c;
}

// One advance edge; returns the lanes that fired, cell i in bit i.
static uint8_t advance(Automaton& node, BusManager& bus, uint32_t& now){
    bus.gate_write(GATE_ADVANCE, false);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;

    bus.gate_write(GATE_ADVANCE, true);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;

    uint8_t fired = 0;
    for (uint8_t i = 0; i < Automaton::LANES; i++){
        if (bus.gate_read((uint8_t)(LANE_BASE + i))) fired = (uint8_t)(fired | (uint8_t)(1u << i));
    }
    return fired;
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

// Rule 90 is left XOR right, and from one cell with silence either side it
// draws a Sierpinski triangle. These are the rows it draws, worked out by
// hand: an exact test of the whole neighbourhood lookup, edges included.
static void test_rule_ninety_draws_the_triangle_it_should() {
    BusManager bus;
    NodeConfig c = automaton_config(90, 1u << 3, Automaton::CA_DEAD,
                                    Automaton::CA_REVIVE_OFF, 8);
    Automaton node(c);
    uint32_t now = 0;

    TEST_ASSERT_EQUAL_HEX8(0x08, node.row());          // the seed: cell 3
    const uint8_t rows[5] = {
        0x14,   // cells 2 and 4
        0x22,   // cells 1 and 5
        0x55,   // cells 0, 2, 4 and 6
        0x80,   // cell 7 - the left half has cancelled itself out
        0x40,   // cell 6
    };
    for (uint8_t g = 0; g < 5; g++){
        advance(node, bus, now);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(rows[g], node.row(), "rule 90 drew the wrong row");
    }
}

static void test_the_lanes_that_fire_are_the_cells_that_live() {
    BusManager bus;
    NodeConfig c = automaton_config(110, 1u << 7, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    Automaton node(c);
    uint32_t now = 0;
    for (uint8_t g = 0; g < 40; g++){
        const uint8_t fired = advance(node, bus, now);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(node.row(), fired, "a lane disagreed with its cell");
    }
}

// Eight Euclidean sequencers give eight patterns with nothing to do with each
// other. Here a cell can only be switched on by a neighbour, so activity
// spreads outward one lane per generation - which is what makes the eight
// lanes a rhythm section rather than eight sequencers.
static void test_activity_spreads_to_neighbours_and_no_further() {
    BusManager bus;
    NodeConfig c = automaton_config(90, 1u << 3, Automaton::CA_DEAD,
                                    Automaton::CA_REVIVE_OFF, 8);
    Automaton node(c);
    uint32_t now = 0;

    // The seed is cell 3. After g generations nothing outside cells 3-g .. 3+g
    // can be alive, whatever the rule has done inside that span.
    for (uint8_t g = 1; g <= 4; g++){
        advance(node, bus, now);
        const int16_t low = (int16_t)3 - (int16_t)g;
        const int16_t high = (int16_t)3 + (int16_t)g;
        for (int16_t i = 0; i < 8; i++){
            if (i >= low && i <= high) continue;
            TEST_ASSERT_TRUE_MESSAGE(!((node.row() >> i) & 1u), "a cell lit out of reach");
        }
    }
}

static void test_a_ring_and_dead_edges_are_different_instruments() {
    BusManager bus;
    NodeConfig ring = automaton_config(90, 1u << 3, Automaton::CA_RING,
                                       Automaton::CA_REVIVE_OFF, 8);
    NodeConfig dead = automaton_config(90, 1u << 3, Automaton::CA_DEAD,
                                       Automaton::CA_REVIVE_OFF, 8);
    Automaton a(ring), b(dead);
    uint32_t now_a = 0, now_b = 0;

    // Three generations in they still agree - the triangle has not reached an
    // edge yet - and the fourth is where the wrap shows up.
    for (uint8_t g = 0; g < 3; g++){
        advance(a, bus, now_a);
        advance(b, bus, now_b);
        TEST_ASSERT_EQUAL_HEX8(a.row(), b.row());
    }
    advance(a, bus, now_a);
    advance(b, bus, now_b);
    TEST_ASSERT_TRUE_MESSAGE(a.row() != b.row(), "the ring never wrapped");
}

// ---------------------------------------------------------------------------
// Coming back from the dead
// ---------------------------------------------------------------------------

static void test_a_row_that_will_never_change_again_is_reseeded() {
    BusManager bus;
    // A full ring under rule 90: every cell has two live neighbours, so
    // left XOR right is zero everywhere and the row empties in one
    // generation. Rule 90 maps an empty neighbourhood to empty, so that is
    // where it would stay for ever.
    NodeConfig c = automaton_config(90, 0xFF, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    Automaton node(c);
    uint32_t now = 0;

    advance(node, bus, now);
    TEST_ASSERT_EQUAL_HEX8(0x00, node.row());
    TEST_ASSERT_EQUAL_UINT32(0, node.revivals());

    // The next generation would be empty again - a row that does not change -
    // so the seed comes back instead.
    advance(node, bus, now);
    TEST_ASSERT_EQUAL_HEX8(0xFF, node.row());
    TEST_ASSERT_EQUAL_UINT32(1, node.revivals());
}

static void test_revive_off_lets_it_die() {
    BusManager bus;
    NodeConfig c = automaton_config(90, 0xFF, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_OFF, 8);
    Automaton node(c);
    uint32_t now = 0;
    advance(node, bus, now);
    for (uint8_t g = 0; g < 50; g++){
        TEST_ASSERT_EQUAL_HEX8(0x00, advance(node, bus, now));
    }
    TEST_ASSERT_EQUAL_UINT32(0, node.revivals());
}

// Blinking is not dying. A row that alternates between two states changes
// every generation, so revive must leave it alone.
static void test_a_blinking_row_is_left_alone() {
    BusManager bus;
    // Rule 51 is "not self": every cell inverts every generation, whatever
    // its neighbours are doing. The row therefore changes every time and is
    // never a fixed point, so revive must leave it alone - blinking is a
    // rhythm, not a death.
    NodeConfig c = automaton_config(51, 0x0F, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    Automaton node(c);
    uint32_t now = 0;
    for (uint8_t g = 0; g < 40; g++){
        advance(node, bus, now);
        TEST_ASSERT_EQUAL_HEX8((g & 1) ? 0x0F : 0xF0, node.row());
    }
    TEST_ASSERT_EQUAL_UINT32(0, node.revivals());
}

static void test_reseed_reloads_the_row_now() {
    BusManager bus;
    NodeConfig c = automaton_config(110, 0x81, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    Automaton node(c);
    uint32_t now = 0;
    for (uint8_t g = 0; g < 9; g++) advance(node, bus, now);
    TEST_ASSERT_TRUE_MESSAGE(node.row() != 0x81, "rule 110 sat still");

    bus.gate_write(GATE_RESEED, true);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;
    TEST_ASSERT_EQUAL_HEX8(0x81, node.row());
}

// ---------------------------------------------------------------------------
// The controls
// ---------------------------------------------------------------------------

static void test_a_shorter_ring_leaves_the_cells_past_it_dark() {
    BusManager bus;
    NodeConfig c = automaton_config(110, 0xFF, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 5);
    Automaton node(c);
    uint32_t now = 0;
    TEST_ASSERT_EQUAL_HEX8(0x1F, node.row());          // the seed, masked to five
    for (uint8_t g = 0; g < 40; g++){
        const uint8_t fired = advance(node, bus, now);
        TEST_ASSERT_TRUE_MESSAGE((node.row() & 0xE0u) == 0, "a cell outside the ring lived");
        TEST_ASSERT_TRUE_MESSAGE((fired & 0xE0u) == 0, "a lane outside the ring fired");
    }
}

static void test_shortening_the_ring_and_lengthening_it_gives_the_seed_back() {
    BusManager bus;
    NodeConfig c = automaton_config(110, 0xA5, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    Automaton node(c);
    TEST_ASSERT_EQUAL_HEX8(0xA5, node.row());
    TEST_ASSERT_TRUE(node.set_param(4, 4));
    TEST_ASSERT_EQUAL_HEX8(0x05, node.row());
    TEST_ASSERT_TRUE(node.set_param(4, 8));

    // The seed is masked, never truncated, so reseeding after the ring has
    // been shortened and grown again gives back what was stored.
    BusManager other;
    uint32_t now = 0;
    other.gate_write(GATE_RESEED, true);
    other.swap();
    node.process(other, now);
    other.swap();
    TEST_ASSERT_EQUAL_HEX8(0xA5, node.row());
    (void)bus;
}

static void test_probability_thins_a_lane_without_changing_the_automaton() {
    BusManager bus;
    NodeConfig c = automaton_config(110, 0x81, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    c.params[5] = 30;
    Automaton node(c);
    uint32_t now = 0;

    uint32_t alive = 0, fired_total = 0;
    for (uint32_t g = 0; g < 400; g++){
        const uint8_t fired = advance(node, bus, now);
        for (uint8_t i = 0; i < 8; i++){
            if ((node.row() >> i) & 1u) alive++;
            if ((fired >> i) & 1u) fired_total++;
            // A lane can never fire for a cell that is not alive.
            TEST_ASSERT_TRUE_MESSAGE(!(((fired >> i) & 1u) && !((node.row() >> i) & 1u)),
                                     "a dead cell fired");
        }
    }
    TEST_ASSERT_TRUE(alive > 0);
    // Roughly three in ten of the live cells reach a jack, and the automaton
    // itself is untouched - the thinning is at the outlet, not in the rule.
    TEST_ASSERT_TRUE(fired_total > alive / 10 && fired_total < alive / 2);
}

static void test_an_unpatched_lane_still_feeds_its_neighbours() {
    BusManager bus;
    NodeConfig wired = automaton_config(110, 0x81, Automaton::CA_RING,
                                        Automaton::CA_REVIVE_ON, 8);
    NodeConfig sparse = wired;
    for (uint8_t i = 3; i < Automaton::LANES; i++) sparse.out_bus[i] = NO_BUS;
    Automaton a(wired), b(sparse);
    uint32_t now_a = 0, now_b = 0;
    for (uint8_t g = 0; g < 40; g++){
        advance(a, bus, now_a);
        advance(b, bus, now_b);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(a.row(), b.row(), "a cell nobody heard stopped living");
    }
}

static void test_the_automaton_never_allocates() {
    const size_t before = g_allocations;
    BusManager bus;
    NodeConfig c = automaton_config(110, 0x81, Automaton::CA_RING,
                                    Automaton::CA_REVIVE_ON, 8);
    Automaton node(c);
    uint32_t now = 0;
    for (uint32_t g = 0; g < 500; g++) advance(node, bus, now);
    TEST_ASSERT_EQUAL_size_t(before, g_allocations);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_rule_ninety_draws_the_triangle_it_should);
    RUN_TEST(test_the_lanes_that_fire_are_the_cells_that_live);
    RUN_TEST(test_activity_spreads_to_neighbours_and_no_further);
    RUN_TEST(test_a_ring_and_dead_edges_are_different_instruments);
    RUN_TEST(test_a_row_that_will_never_change_again_is_reseeded);
    RUN_TEST(test_revive_off_lets_it_die);
    RUN_TEST(test_a_blinking_row_is_left_alone);
    RUN_TEST(test_reseed_reloads_the_row_now);
    RUN_TEST(test_a_shorter_ring_leaves_the_cells_past_it_dark);
    RUN_TEST(test_shortening_the_ring_and_lengthening_it_gives_the_seed_back);
    RUN_TEST(test_probability_thins_a_lane_without_changing_the_automaton);
    RUN_TEST(test_an_unpatched_lane_still_feeds_its_neighbours);
    RUN_TEST(test_the_automaton_never_allocates);
    return UNITY_END();
}
