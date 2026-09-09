#include <unity.h>
#include <stdio.h>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "algorithm/logic/gates.h"
#include "algorithm/switch/sustain.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Logic gates
// ---------------------------------------------------------------------------

static const uint16_t IN_AB  = 0b00000011;   // ports 0 and 1
static const uint16_t OUT_C  = 0b00000100;   // port 2

// Runs the four two-input combinations (a, b) = (0,0) (1,0) (0,1) (1,1)
// through `gate` and compares port 2 with `expected`.
static void check_truth_table(FakeGpio& gpio, Algorithm& gate, const uint8_t expected[4], const char* name) {
    gate.setup();
    for (uint8_t combo = 0; combo < 4; combo++) {
        gpio.set_input(0, combo & 1);
        gpio.set_input(1, (combo >> 1) & 1);
        gate.update(0);
        char msg[48];
        snprintf(msg, sizeof msg, "%s a=%u b=%u", name, combo & 1, (combo >> 1) & 1);
        TEST_ASSERT_EQUAL_MESSAGE(expected[combo], gpio.outputs[2], msg);
    }
}

static void test_and_truth_table()  { FakeGpio g; LogicAND  x(g, IN_AB, OUT_C); const uint8_t e[4] = {0, 0, 0, 1}; check_truth_table(g, x, e, "AND"); }
static void test_nand_truth_table() { FakeGpio g; LogicNAND x(g, IN_AB, OUT_C); const uint8_t e[4] = {1, 1, 1, 0}; check_truth_table(g, x, e, "NAND"); }
static void test_or_truth_table()   { FakeGpio g; LogicOR   x(g, IN_AB, OUT_C); const uint8_t e[4] = {0, 1, 1, 1}; check_truth_table(g, x, e, "OR"); }
static void test_nor_truth_table()  { FakeGpio g; LogicNOR  x(g, IN_AB, OUT_C); const uint8_t e[4] = {1, 0, 0, 0}; check_truth_table(g, x, e, "NOR"); }
static void test_xor_truth_table()  { FakeGpio g; LogicXOR  x(g, IN_AB, OUT_C); const uint8_t e[4] = {0, 1, 1, 0}; check_truth_table(g, x, e, "XOR"); }
static void test_xnor_truth_table() { FakeGpio g; LogicXNOR x(g, IN_AB, OUT_C); const uint8_t e[4] = {1, 0, 0, 1}; check_truth_table(g, x, e, "XNOR"); }

static void test_not_truth_table() {
    FakeGpio gpio;
    LogicNot gate(gpio, 0b00000001, 0b00011110);
    gate.setup();
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT_PULLUP, gpio.modes[0]);
    for (uint8_t port = 1; port <= 4; port++) TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, gpio.modes[port]);

    // FakeGpio reports the input HIGH, LogicNot runs, every output in the mask goes LOW.
    gpio.set_input(0, GPIO_HIGH);
    gpio.clear_writes();
    gate.update(0);
    TEST_ASSERT_EQUAL(4, gpio.writes.size());
    for (uint8_t port = 1; port <= 4; port++) TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[port]);

    gpio.set_input(0, GPIO_LOW);
    gate.update(0);
    for (uint8_t port = 1; port <= 4; port++) TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[port]);
}

// XOR over more than two inputs is parity (documented decision).
static void test_xor_three_inputs_is_parity() {
    FakeGpio gpio;
    LogicXOR gate(gpio, 0b00000111, 0b00001000);
    gate.setup();
    for (uint8_t combo = 0; combo < 8; combo++) {
        uint8_t ones = 0;
        for (uint8_t i = 0; i < 3; i++) { gpio.set_input(i, (combo >> i) & 1); ones += (combo >> i) & 1; }
        gate.update(0);
        TEST_ASSERT_EQUAL(ones & 1, gpio.outputs[3]);
    }
}

// An unpatched input reads 0 after normalisation, so it must not force an OR high.
static void test_or_with_unpatched_input_follows_the_patched_one() {
    FakeGpio gpio;
    LogicOR gate(gpio, IN_AB, OUT_C);
    gate.setup();
    gpio.set_input(1, GPIO_LOW);             // unpatched
    gpio.set_input(0, GPIO_LOW); gate.update(0); TEST_ASSERT_EQUAL(0, gpio.outputs[2]);
    gpio.set_input(0, GPIO_HIGH); gate.update(0); TEST_ASSERT_EQUAL(1, gpio.outputs[2]);
}

// ---------------------------------------------------------------------------
// Algorithm lifecycle and configuration contract
// ---------------------------------------------------------------------------

static void test_zero_input_mask_is_rejected_and_never_touches_hardware() {
    FakeGpio gpio;
    LogicNot gate(gpio, 0, 0b00000010);
    TEST_ASSERT_FALSE(gate.is_valid());
    gate.setup();
    gate.update(0);
    TEST_ASSERT_EQUAL(0, gpio.writes.size());
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, gpio.modes[1]);   // output never claimed
}

static void test_multi_bit_input_mask_is_rejected_for_single_input_algorithms() {
    FakeGpio gpio;
    RecordingMidiOut midi;
    LogicNot gate(gpio, 0b00000101, 0b00000010);
    Sustain sustain(gpio, midi, 0b11000000, 0xFF);
    TEST_ASSERT_FALSE(gate.is_valid());
    TEST_ASSERT_FALSE(sustain.is_valid());
    sustain.setup();
    TEST_ASSERT_EQUAL(0, midi.messages.size());
}

static void test_single_port_helper() {
    TEST_ASSERT_EQUAL(NO_PORT, Algorithm::single_port(0));
    TEST_ASSERT_EQUAL(0, Algorithm::single_port(0b00000001));
    TEST_ASSERT_EQUAL(7, Algorithm::single_port(0b10000000));
    TEST_ASSERT_EQUAL(NO_PORT, Algorithm::single_port(0b10000001));
}

static void test_nothing_is_driven_before_setup() {
    FakeGpio gpio;
    LogicNot gate(gpio, 0b00000001, 0b00000010);
    TEST_ASSERT_EQUAL(0, gpio.writes.size());
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, gpio.modes[1]);
    gate.setup();
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, gpio.modes[1]);
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[1]);
}

// Destructor returns the claimed ports to inputs, and combines the masks with
// OR: with input 0b0011 and output 0b0010, `+` would have produced 0b0101 and
// reset port 2, which belongs to nobody here.
static void test_destructor_releases_claimed_ports_only() {
    FakeGpio gpio;
    gpio.modes[2] = GPIO_MODE_OUTPUT;   // owned by someone else
    {
        LogicAND gate(gpio, 0b00000011, 0b00000010);
        gate.setup();
        TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, gpio.modes[1]);
    }
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT_PULLUP, gpio.modes[0]);
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT_PULLUP, gpio.modes[1]);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, gpio.modes[2]);
}

static void test_bypass_stops_the_algorithm() {
    FakeGpio gpio;
    LogicNot gate(gpio, 0b00000001, 0b00000010);
    gate.setup();
    gate.set_bypass(true);
    gpio.clear_writes();
    gate.update(0);
    TEST_ASSERT_EQUAL(0, gpio.writes.size());
}

// ---------------------------------------------------------------------------
// Sustain
// ---------------------------------------------------------------------------

static void test_sustain_transmits_initial_state_on_setup() {
    FakeGpio gpio;
    RecordingMidiOut midi;
    Sustain sustain(gpio, midi, 0b10000000, 0x31);
    gpio.set_input(7, GPIO_HIGH);            // pedal already down at boot
    sustain.setup();
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(0x31, midi.messages[0].target);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, midi.messages[0].type);
    TEST_ASSERT_EQUAL(64, midi.messages[0].d1);
    TEST_ASSERT_EQUAL(127, midi.messages[0].d2);
    TEST_ASSERT_EQUAL(1, midi.messages[0].channel);
}

static void test_sustain_sends_on_debounced_change_only() {
    FakeGpio gpio;
    RecordingMidiOut midi;
    Sustain sustain(gpio, midi, 0b10000000, 0xFF);
    gpio.set_input(7, GPIO_LOW);
    sustain.setup();                          // initial: up
    midi.clear();

    // Bounce: toggle every 1 ms for 20 ms, faster than the 5 ms debounce.
    uint32_t now = 0;
    for (int i = 0; i < 20; i++) {
        gpio.set_input(7, i & 1);
        now += 1000;
        sustain.update(now);
    }
    TEST_ASSERT_EQUAL(0, midi.messages.size());

    // Settle pressed; sample every 1 ms for 10 ms.
    gpio.set_input(7, GPIO_HIGH);
    for (int i = 0; i < 10; i++) { now += 1000; sustain.update(now); }
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(127, midi.messages[0].d2);

    // Held: nothing more.
    for (int i = 0; i < 100; i++) { now += 1000; sustain.update(now); }
    TEST_ASSERT_EQUAL(1, midi.messages.size());
}

static void test_sustain_channel_controller_and_invert_are_configurable() {
    FakeGpio gpio;
    RecordingMidiOut midi;
    Sustain sustain(gpio, midi, 0b00000001, 0x01);
    sustain.set_channel(5);
    sustain.set_controller(66);
    sustain.set_invert(true);
    gpio.set_input(0, GPIO_LOW);              // normally-closed pedal: open contact = down
    sustain.setup();
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(5, midi.messages[0].channel);
    TEST_ASSERT_EQUAL(66, midi.messages[0].d1);
    TEST_ASSERT_EQUAL(127, midi.messages[0].d2);
}

// Timestamps are microseconds that wrap; the debounce must survive the wrap.
static void test_sustain_debounce_survives_timer_wrap() {
    FakeGpio gpio;
    RecordingMidiOut midi;
    Sustain sustain(gpio, midi, 0b00000001, 0x01);
    gpio.set_input(0, GPIO_LOW);
    sustain.setup();
    midi.clear();
    uint32_t now = 0xFFFFFFFFu - 2000;
    gpio.set_input(0, GPIO_HIGH);
    sustain.update(now);                      // change seen
    now += 1000; sustain.update(now);
    now += 6000; sustain.update(now);         // wrapped, 7 ms later
    TEST_ASSERT_EQUAL(1, midi.messages.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_and_truth_table);
    RUN_TEST(test_nand_truth_table);
    RUN_TEST(test_or_truth_table);
    RUN_TEST(test_nor_truth_table);
    RUN_TEST(test_xor_truth_table);
    RUN_TEST(test_xnor_truth_table);
    RUN_TEST(test_not_truth_table);
    RUN_TEST(test_xor_three_inputs_is_parity);
    RUN_TEST(test_or_with_unpatched_input_follows_the_patched_one);
    RUN_TEST(test_zero_input_mask_is_rejected_and_never_touches_hardware);
    RUN_TEST(test_multi_bit_input_mask_is_rejected_for_single_input_algorithms);
    RUN_TEST(test_single_port_helper);
    RUN_TEST(test_nothing_is_driven_before_setup);
    RUN_TEST(test_destructor_releases_claimed_ports_only);
    RUN_TEST(test_bypass_stops_the_algorithm);
    RUN_TEST(test_sustain_transmits_initial_state_on_setup);
    RUN_TEST(test_sustain_sends_on_debounced_change_only);
    RUN_TEST(test_sustain_channel_controller_and_invert_are_configurable);
    RUN_TEST(test_sustain_debounce_survives_timer_wrap);
    return UNITY_END();
}
