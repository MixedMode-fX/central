#include <unity.h>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "algorithm/logic/gates.h"
#include "algorithm/switch/sustain.h"

void setUp() {}
void tearDown() {}

// FakeGpio reports the input HIGH, LogicNot runs, and every output in the
// mask is driven LOW.
static void test_logic_not_drives_every_output_low_when_input_high() {
    FakeGpio gpio;
    LogicNot gate(gpio, 0, 0, 0b00000001, 0b00011110);

    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT_PULLUP, gpio.modes[0]);
    for (uint8_t port = 1; port <= 4; port++) TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, gpio.modes[port]);

    gpio.set_input(0, GPIO_HIGH);
    gpio.clear_writes();
    gate.update();

    TEST_ASSERT_EQUAL(4, gpio.writes.size());
    for (uint8_t port = 1; port <= 4; port++) TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[port]);

    gpio.set_input(0, GPIO_LOW);
    gate.update();
    for (uint8_t port = 1; port <= 4; port++) TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[port]);
}

static void test_logic_and_two_inputs() {
    FakeGpio gpio;
    LogicAND gate(gpio, 0, 0, 0b00000011, 0b00000100);
    const uint8_t expected[4] = {0, 0, 0, 1};
    for (uint8_t combo = 0; combo < 4; combo++) {
        gpio.set_input(0, combo & 1);
        gpio.set_input(1, (combo >> 1) & 1);
        gate.update();
        TEST_ASSERT_EQUAL(expected[combo], gpio.outputs[2]);
    }
}

static void test_bypass_stops_the_algorithm() {
    FakeGpio gpio;
    LogicNot gate(gpio, 0, 0, 0b00000001, 0b00000010);
    gate.set_bypass(true);
    gpio.clear_writes();
    gate.update();
    TEST_ASSERT_EQUAL(0, gpio.writes.size());
}

static void test_sustain_sends_cc64_to_target_mask_on_change() {
    FakeGpio gpio;
    RecordingMidiOut midi;
    Sustain sustain(gpio, midi, 0, 0x31, 0b10000000, 0);

    gpio.set_input(7, GPIO_HIGH);   // idle: pull-up, pedal not pressed
    sustain.update();
    TEST_ASSERT_EQUAL(0, midi.messages.size());

    gpio.set_input(7, GPIO_LOW);    // pedal pressed
    sustain.update();
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(0x31, midi.messages[0].target);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, midi.messages[0].type);
    TEST_ASSERT_EQUAL(64, midi.messages[0].d1);
    TEST_ASSERT_EQUAL(64, midi.messages[0].d2);
    TEST_ASSERT_EQUAL(1, midi.messages[0].channel);

    sustain.update();               // no change, no message
    TEST_ASSERT_EQUAL(1, midi.messages.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_logic_not_drives_every_output_low_when_input_high);
    RUN_TEST(test_logic_and_two_inputs);
    RUN_TEST(test_bypass_stops_the_algorithm);
    RUN_TEST(test_sustain_sends_cc64_to_target_mask_on_change);
    return UNITY_END();
}
