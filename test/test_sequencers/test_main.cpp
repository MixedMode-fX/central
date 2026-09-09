#include <unity.h>
#include <stdio.h>
#include <string>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "master.h"
#include "algorithm/sequencer/sequencers.h"
#include "algorithm/sequencer/euclid.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Euclidean patterns, against the published references
// ---------------------------------------------------------------------------

static std::string pattern_string(uint32_t bits, uint8_t steps) {
    std::string s;
    for (uint8_t i = 0; i < steps; i++) s += (bits & ((uint32_t)1u << i)) ? '1' : '0';
    return s;
}

static void check_euclid(uint8_t k, uint8_t n, const char* expected) {
    const std::string got = pattern_string(euclidean_pattern(k, n), n);
    char message[80];
    snprintf(message, sizeof message, "E(%u,%u) = %s", k, n, got.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, got.c_str(), message);
}

static void test_euclidean_patterns_match_the_references() {
    check_euclid(3, 8,  "10010010");
    check_euclid(5, 8,  "10110110");
    check_euclid(4, 16, "1000100010001000");
    check_euclid(7, 16, "1001010100101010");
    check_euclid(2, 5,  "10100");
    check_euclid(5, 12, "100101001010");
    check_euclid(1, 4,  "1000");
}

static void test_euclidean_edge_cases() {
    check_euclid(0, 8, "00000000");        // no pulses is silence, not a crash
    check_euclid(8, 8, "11111111");        // every step
    check_euclid(9, 8, "11111111");        // more pulses than steps is clamped
    TEST_ASSERT_EQUAL_UINT32(0, euclidean_pattern(3, 0));
    TEST_ASSERT_EQUAL_UINT32(0, euclidean_pattern(3, MAX_SEQUENCE_LEN + 1));
    // Every pattern has exactly as many pulses as it was asked for.
    for (uint8_t n = 1; n <= MAX_SEQUENCE_LEN; n++) {
        for (uint8_t k = 0; k <= n; k++) {
            uint8_t count = 0;
            const uint32_t bits = euclidean_pattern(k, n);
            for (uint8_t i = 0; i < n; i++) if (bits & ((uint32_t)1u << i)) count++;
            TEST_ASSERT_EQUAL(k, count);
        }
    }
}

static void test_rotation_is_separate_from_the_pattern() {
    const uint32_t base = euclidean_pattern(3, 8);
    TEST_ASSERT_EQUAL_STRING("10010010", pattern_string(base, 8).c_str());
    // Rotation starts the pattern that many steps in, so step 0 of the
    // rotated pattern is step `by` of the original.
    TEST_ASSERT_EQUAL_STRING("00100101", pattern_string(rotate_pattern(base, 8, 1), 8).c_str());
    TEST_ASSERT_EQUAL_STRING("10010100", pattern_string(rotate_pattern(base, 8, 3), 8).c_str());
    TEST_ASSERT_EQUAL_STRING("10010010", pattern_string(rotate_pattern(base, 8, 8), 8).c_str());
}

// ---------------------------------------------------------------------------
// The sequencers, driven from a fake gate source
// ---------------------------------------------------------------------------

// One advance edge: a rising edge, then a falling one, collecting the output.
// Two passes per edge, 20 ms apart, so a 5 ms trigger is up on the first and
// down by the next edge.
static bool advance_once(BusManager& bus, Node& node, uint8_t advance_bus,
                         uint8_t out_bus, uint32_t& now) {
    bus.gate_write(advance_bus, true);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    const bool fired = bus.gate_read(out_bus);
    now += 20000;

    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 20000;
    return fired;
}

static std::string run_sequence(Node& node, uint8_t steps, uint8_t advance_bus = 0,
                                uint8_t out_bus = 1) {
    BusManager bus;
    uint32_t now = 0;
    std::string out;
    for (uint8_t i = 0; i < steps; i++) out += advance_once(bus, node, advance_bus, out_bus, now) ? '1' : '0';
    return out;
}

static NodeConfig seq_config(uint8_t id, uint8_t length) {
    NodeConfig c = node_config(id);
    c.in_bus[0] = 0;
    c.out_bus[0] = 1;
    c.params[0] = length;
    return c;
}

// Every edge passes through: the divider upstream sets the rate.
static void test_metronome_fires_on_every_edge() {
    NodeConfig c = seq_config(ALGO_METRONOME, 1);
    Metronome node(c);
    TEST_ASSERT_EQUAL_STRING("11111111", run_sequence(node, 8).c_str());
}

// Length 5 wraps after step 5, not after 16.
static void test_step_sequencer_wraps_at_its_length() {
    NodeConfig c = seq_config(ALGO_STEP_SEQ, 5);
    c.params[3] = 0b10011;                       // steps 0, 1 and 4
    StepSequencer node(c);
    TEST_ASSERT_EQUAL(5, node.length());
    // Two full cycles, to prove the wrap and not just the first pass. A
    // sequencer that wrapped at 16 would give ten steps of 1100100000.
    TEST_ASSERT_EQUAL_STRING("1100111001", run_sequence(node, 10).c_str());
}

// All MAX_SEQUENCE_LEN steps are reachable, four bytes of preset.
static void test_step_sequencer_uses_the_whole_length() {
    NodeConfig c = seq_config(ALGO_STEP_SEQ, MAX_SEQUENCE_LEN);
    c.params[3] = 0x01; c.params[4] = 0x00; c.params[5] = 0x00; c.params[6] = 0x80;
    StepSequencer node(c);
    std::string expected(MAX_SEQUENCE_LEN, '0');
    expected[0] = '1';
    expected[MAX_SEQUENCE_LEN - 1] = '1';
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), run_sequence(node, MAX_SEQUENCE_LEN).c_str());
}

static void test_euclidian_sequencer_plays_its_pattern_twice_over() {
    NodeConfig c = seq_config(ALGO_EUCLID_SEQ, 8);
    c.params[3] = 3;
    EuclidianSequencer node(c);
    TEST_ASSERT_EQUAL_STRING("1001001010010010", run_sequence(node, 16).c_str());

    NodeConfig r = seq_config(ALGO_EUCLID_SEQ, 8);
    r.params[3] = 3;
    r.params[4] = 1;                             // rotated one step
    EuclidianSequencer rotated(r);
    TEST_ASSERT_EQUAL_STRING("0010010100100101", run_sequence(rotated, 16).c_str());
}

// Direction is cheap here and #13 inherits it.
static void test_directions() {
    const char* expected[4] = {
        "10011001",        // forward:   0 1 2 3 0 1 2 3
        "10011001",        // reverse:   3 2 1 0 3 2 1 0 - starts on the last step
        "10010010",        // ping-pong: 0 1 2 3 2 1 0 1, endpoints not repeated
        nullptr,           // random: asserted below
    };
    for (uint8_t dir = 0; dir < 3; dir++) {
        NodeConfig c = seq_config(ALGO_STEP_SEQ, 4);
        c.params[1] = dir;
        c.params[3] = 0b1001;                    // steps 0 and 3
        StepSequencer node(c);
        char message[32];
        snprintf(message, sizeof message, "direction %u", dir);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expected[dir], run_sequence(node, 8).c_str(), message);
    }
    // Random visits steps that are on and steps that are off, and stays in range.
    NodeConfig c = seq_config(ALGO_STEP_SEQ, 4);
    c.params[1] = GateSequencer::SEQ_RANDOM;
    c.params[3] = 0b1001;
    StepSequencer node(c);
    const std::string out = run_sequence(node, 200);
    TEST_ASSERT_TRUE(out.find('1') != std::string::npos);
    TEST_ASSERT_TRUE(out.find('0') != std::string::npos);
    TEST_ASSERT_TRUE(node.position() < 4);
}

// A pulse on the reset inlet returns the sequencer to step 0 on the next
// advance - identically in all four.
static void test_reset_returns_to_step_zero() {
    // All four take a reset inlet, and it is the same inlet in each.
    for (uint8_t id = ALGO_METRONOME; id <= ALGO_RANDOM_SEQ; id++) {
        NodeConfig c = seq_config(id, 4);
        c.in_bus[1] = 2;
        TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
        const AlgorithmDescriptor* d = registry::find(id);
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_EQUAL(Domain::Gate, d->in_domain[1]);
        TEST_ASSERT_EQUAL(1, d->min_in);            // only advance is required
    }

    // Driven concretely on the one whose pattern makes the position visible.
    NodeConfig c = seq_config(ALGO_STEP_SEQ, 4);
    c.in_bus[1] = 2;
    c.params[3] = 0b0001;                           // only step 0 fires
    StepSequencer node(c);

    BusManager bus;
    uint32_t now = 0;
    TEST_ASSERT_TRUE(advance_once(bus, node, 0, 1, now));    // step 0
    TEST_ASSERT_FALSE(advance_once(bus, node, 0, 1, now));   // step 1
    TEST_ASSERT_FALSE(advance_once(bus, node, 0, 1, now));   // step 2

    bus.gate_write(2, true);                                 // reset
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 20000;

    TEST_ASSERT_TRUE(advance_once(bus, node, 0, 1, now));     // step 0 again
    TEST_ASSERT_EQUAL(0, node.position());
}

// A random pattern differs between power cycles and holds still until shredded.
static void test_random_sequencer_shred_and_power_cycles() {
    entropy::stir(0xDEADBEEF);                   // "power cycle" one
    NodeConfig c = seq_config(ALGO_RANDOM_SEQ, 16);
    c.params[3] = 50;
    RandomSequencer first(c);
    const uint32_t first_pattern = first.pattern();

    entropy::stir(0x0BADF00D);                   // "power cycle" two
    RandomSequencer second(c);
    TEST_ASSERT_NOT_EQUAL(first_pattern, second.pattern());

    // The same pattern until shredded, whatever the sequencer is asked to play.
    const std::string a = run_sequence(second, 16);
    const std::string b = run_sequence(second, 16);
    TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());

    second.shred();
    TEST_ASSERT_NOT_EQUAL(first_pattern, second.pattern());
    // Density 100 and density 0 are the two ends, and both are honoured.
    NodeConfig dense = seq_config(ALGO_RANDOM_SEQ, 8);
    dense.params[3] = 100;
    RandomSequencer all_on(dense);
    TEST_ASSERT_EQUAL_STRING("11111111", run_sequence(all_on, 8).c_str());
}

// The shred inlet: another node throws the pattern away mid-performance.
static void test_random_sequencer_shred_inlet() {
    NodeConfig c = seq_config(ALGO_RANDOM_SEQ, 16);
    c.in_bus[2] = 3;
    c.params[3] = 50;
    RandomSequencer node(c);
    const uint32_t before = node.pattern();

    BusManager bus;
    uint32_t now = 0;
    bus.gate_write(3, true);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    TEST_ASSERT_NOT_EQUAL(before, node.pattern());
}

// No sequencer allocates after construction, and none names a pin.
static void test_sequencers_fit_a_pool_slot() {
    TEST_ASSERT_TRUE(sizeof(Metronome) <= NODE_SLOT_SIZE);
    TEST_ASSERT_TRUE(sizeof(StepSequencer) <= NODE_SLOT_SIZE);
    TEST_ASSERT_TRUE(sizeof(EuclidianSequencer) <= NODE_SLOT_SIZE);
    TEST_ASSERT_TRUE(sizeof(RandomSequencer) <= NODE_SLOT_SIZE);
}

// ---------------------------------------------------------------------------
// Through the master: sequencers behind dividers, and behind a logic gate
// ---------------------------------------------------------------------------

// Two sequencers on two dividers stay phase-locked to the master over 10,000
// ticks: every /4 pulse of the fast one lands on a tick the slow one agrees
// with, with no accumulated slip.
static void test_two_sequencers_behind_dividers_stay_locked() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);            // tick -> gate 0, /1
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 1;
    p.nodes[1] = node_config(ALGO_CLOCK_DIV);            // tick -> gate 1, /4
    p.nodes[1].out_bus[0] = 1;
    p.nodes[1].params[1] = 4;
    p.nodes[2] = node_config(ALGO_METRONOME);            // gate 0 -> gate 2
    p.nodes[2].in_bus[0] = 0; p.nodes[2].out_bus[0] = 2;
    p.nodes[3] = node_config(ALGO_METRONOME);            // gate 1 -> gate 3
    p.nodes[3].in_bus[0] = 1; p.nodes[3].out_bus[0] = 3;
    p.n_nodes = 4;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 2};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 3};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t fast = 0, slow = 0;
    bool was_fast = false, was_slow = false;
    uint32_t now = 0;
    for (uint32_t t = 0; t < 10000u * CLOCK_SUBTICK; t++) {
        master.clock().advance();
        master.pass(now);
        const bool f = gpio.outputs[0] == GPIO_HIGH;
        const bool s = gpio.outputs[1] == GPIO_HIGH;
        if (f && !was_fast) fast++;
        if (s && !was_slow) {
            slow++;
            // The slow one has never taken a step the fast one did not
            // account for: exactly four to one, for 10,000 ticks.
            TEST_ASSERT_EQUAL_UINT32(slow * 4, fast);
        }
        was_fast = f; was_slow = s;
        now += 300;
    }
    // A few passes with no new subticks, so the last divider pulse reaches
    // the jack: each stage of the chain costs one pass, not one tick.
    for (int i = 0; i < 4; i++) {
        master.pass(now);
        const bool f = gpio.outputs[0] == GPIO_HIGH;
        const bool s = gpio.outputs[1] == GPIO_HIGH;
        if (f && !was_fast) fast++;
        if (s && !was_slow) { slow++; TEST_ASSERT_EQUAL_UINT32(slow * 4, fast); }
        was_fast = f; was_slow = s;
        now += 300;
    }
    TEST_ASSERT_EQUAL_UINT32(10000, fast);
    TEST_ASSERT_EQUAL_UINT32(2500, slow);
}

// A sequencer advanced by a plain logic gate, with no ClockDiv anywhere in
// the patch, works identically: the advance inlet does not care what wrote it.
static void test_sequencer_advanced_by_a_logic_gate() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};   // jack 1 -> gate 0
    p.nodes[0] = node_config(ALGO_LOGIC_AND);            // gate 0 -> gate 1
    p.nodes[0].in_bus[0] = 0; p.nodes[0].out_bus[0] = 1;
    p.nodes[1] = node_config(ALGO_STEP_SEQ);             // gate 1 -> gate 2
    p.nodes[1].in_bus[0] = 1; p.nodes[1].out_bus[0] = 2;
    p.nodes[1].params[0] = 4;
    p.nodes[1].params[3] = 0b0101;                       // steps 0 and 2
    p.n_nodes = 2;
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 2};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    gpio.set_input(0, GPIO_LOW);
    for (int i = 0; i < 5; i++) { master.pass(now); now += 20000; }   // settle
    bool was = gpio.outputs[1] == GPIO_HIGH;

    std::string played;
    for (uint8_t edge = 0; edge < 8; edge++) {
        gpio.set_input(0, GPIO_HIGH);                    // one edge through the gate
        bool fired = false;
        for (int i = 0; i < 3; i++) {
            master.pass(now);
            const bool high = gpio.outputs[1] == GPIO_HIGH;
            if (high && !was) fired = true;
            was = high;
            now += 1000;
        }
        gpio.set_input(0, GPIO_LOW);
        for (int i = 0; i < 3; i++) {
            master.pass(now);
            const bool high = gpio.outputs[1] == GPIO_HIGH;
            if (high && !was) fired = true;
            was = high;
            now += 20000;
        }
        played += fired ? '1' : '0';
    }
    TEST_ASSERT_EQUAL_STRING("10101010", played.c_str());
}

// A sequencer resetting another sequencer: the reset inlet is a bus like any
// other, which is most of what makes the model worth having.
static void test_a_sequencer_resets_another_sequencer() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);            // tick -> gate 0
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 1;
    p.nodes[1] = node_config(ALGO_EUCLID_SEQ);           // gate 0 -> gate 1, every 6th
    p.nodes[1].in_bus[0] = 0; p.nodes[1].out_bus[0] = 1;
    p.nodes[1].params[0] = 6; p.nodes[1].params[3] = 1;
    p.nodes[2] = node_config(ALGO_STEP_SEQ);             // gate 0 advance, gate 1 reset
    p.nodes[2].in_bus[0] = 0; p.nodes[2].in_bus[1] = 1; p.nodes[2].out_bus[0] = 2;
    p.nodes[2].params[0] = 4; p.nodes[2].params[3] = 0b0001;
    p.n_nodes = 3;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 2};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t fires = 0;
    bool was = false;
    uint32_t now = 0;
    for (uint32_t t = 0; t < 60u * CLOCK_SUBTICK; t++) {
        master.clock().advance();
        master.pass(now);
        const bool high = gpio.outputs[0] == GPIO_HIGH;
        if (high && !was) fires++;
        was = high;
        now += 300;
    }
    // Left alone the 4-step sequencer would fire 15 times in 60 steps. Reset
    // every 6 steps, it fires more often than that, because being sent back
    // to step 0 makes step 0 come round sooner.
    TEST_ASSERT_TRUE(fires > 15);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_euclidean_patterns_match_the_references);
    RUN_TEST(test_euclidean_edge_cases);
    RUN_TEST(test_rotation_is_separate_from_the_pattern);
    RUN_TEST(test_metronome_fires_on_every_edge);
    RUN_TEST(test_step_sequencer_wraps_at_its_length);
    RUN_TEST(test_step_sequencer_uses_the_whole_length);
    RUN_TEST(test_euclidian_sequencer_plays_its_pattern_twice_over);
    RUN_TEST(test_directions);
    RUN_TEST(test_reset_returns_to_step_zero);
    RUN_TEST(test_random_sequencer_shred_and_power_cycles);
    RUN_TEST(test_random_sequencer_shred_inlet);
    RUN_TEST(test_sequencers_fit_a_pool_slot);
    RUN_TEST(test_two_sequencers_behind_dividers_stay_locked);
    RUN_TEST(test_sequencer_advanced_by_a_logic_gate);
    RUN_TEST(test_a_sequencer_resets_another_sequencer);
    return UNITY_END();
}
