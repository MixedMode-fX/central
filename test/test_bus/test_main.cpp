#include <unity.h>
#include "bus/bus_manager.h"
#include "hal/midi_types.h"

void setUp() {}
void tearDown() {}

static MidiEvent note_on(uint8_t note) { return MidiEvent{MIDI_NOTE_ON, 1, note, 100}; }

// Readers see the previous pass; writes are invisible until swap().
static void test_gate_is_double_buffered() {
    BusManager bus;
    bus.gate_write(3, true);
    TEST_ASSERT_FALSE(bus.gate_read(3));
    bus.swap();
    TEST_ASSERT_TRUE(bus.gate_read(3));
    bus.swap();                       // nobody wrote this pass
    TEST_ASSERT_FALSE(bus.gate_read(3));
}

// Gate fan-in is OR: a passive mult.
static void test_gate_fan_in_is_or() {
    BusManager bus;
    bus.gate_write(0, false);
    bus.gate_write(0, true);
    bus.gate_write(0, false);
    bus.swap();
    TEST_ASSERT_TRUE(bus.gate_read(0));
    bus.gate_write(0, false);
    bus.gate_write(0, false);
    bus.swap();
    TEST_ASSERT_FALSE(bus.gate_read(0));
}

// Note fan-in keeps arrival order, and the queue is drained each pass.
static void test_note_arrival_order_and_per_pass_drain() {
    BusManager bus;
    TEST_ASSERT_TRUE(bus.note_write(2, note_on(60)));
    TEST_ASSERT_TRUE(bus.note_write(2, note_on(64)));
    TEST_ASSERT_TRUE(bus.note_write(2, note_on(67)));
    TEST_ASSERT_EQUAL(0, bus.note_count(2));
    bus.swap();
    TEST_ASSERT_EQUAL(3, bus.note_count(2));
    TEST_ASSERT_EQUAL(60, bus.note_read(2, 0).data1);
    TEST_ASSERT_EQUAL(64, bus.note_read(2, 1).data1);
    TEST_ASSERT_EQUAL(67, bus.note_read(2, 2).data1);
    bus.swap();
    TEST_ASSERT_EQUAL(0, bus.note_count(2));
}

// Overflow drops the newest event and counts it - never silently.
static void test_note_overflow_is_counted_not_silent() {
    BusManager bus;
    for (uint8_t i = 0; i < NOTE_QUEUE_DEPTH; i++) TEST_ASSERT_TRUE(bus.note_write(0, note_on(i)));
    TEST_ASSERT_FALSE(bus.note_write(0, note_on(100)));
    TEST_ASSERT_FALSE(bus.note_write(0, note_on(101)));
    TEST_ASSERT_EQUAL(2, bus.note_overflows(0));
    TEST_ASSERT_EQUAL(0, bus.note_overflows(1));
    bus.swap();
    TEST_ASSERT_EQUAL(NOTE_QUEUE_DEPTH, bus.note_count(0));
    TEST_ASSERT_EQUAL(NOTE_QUEUE_DEPTH - 1, bus.note_read(0, NOTE_QUEUE_DEPTH - 1).data1);   // oldest kept
}

// CV fan-in sums with saturation: clamp, don't wrap.
static void test_cv_sum_saturates() {
    BusManager bus;
    bus.cv_write(0, 30000);
    bus.cv_write(0, 30000);
    bus.cv_write(1, -30000);
    bus.cv_write(1, -30000);
    bus.cv_write(2, 100);
    bus.cv_write(2, -250);
    bus.swap();
    TEST_ASSERT_EQUAL(INT16_MAX, bus.cv_read(0));
    TEST_ASSERT_EQUAL(INT16_MIN, bus.cv_read(1));
    TEST_ASSERT_EQUAL(-150, bus.cv_read(2));
    bus.swap();
    TEST_ASSERT_EQUAL(0, bus.cv_read(0));
}

static void test_out_of_range_indices_are_ignored() {
    BusManager bus;
    bus.gate_write(N_GATE_BUS, true);
    bus.cv_write(N_CV_BUS, 5);
    TEST_ASSERT_FALSE(bus.note_write(N_NOTE_BUS, note_on(1)));
    bus.swap();
    TEST_ASSERT_FALSE(bus.gate_read(N_GATE_BUS));
    TEST_ASSERT_EQUAL(0, bus.cv_read(N_CV_BUS));
    TEST_ASSERT_EQUAL(0, bus.note_count(N_NOTE_BUS));
    TEST_ASSERT_EQUAL(0, bus.note_read(0, 5).type);
}

static void test_reset_clears_everything() {
    BusManager bus;
    bus.gate_write(1, true);
    bus.note_write(1, note_on(1));
    bus.cv_write(1, 1);
    bus.swap();
    bus.gate_write(2, true);
    bus.reset();
    bus.swap();
    TEST_ASSERT_FALSE(bus.gate_read(1));
    TEST_ASSERT_FALSE(bus.gate_read(2));
    TEST_ASSERT_EQUAL(0, bus.note_count(1));
    TEST_ASSERT_EQUAL(0, bus.cv_read(1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_gate_is_double_buffered);
    RUN_TEST(test_gate_fan_in_is_or);
    RUN_TEST(test_note_arrival_order_and_per_pass_drain);
    RUN_TEST(test_note_overflow_is_counted_not_silent);
    RUN_TEST(test_cv_sum_saturates);
    RUN_TEST(test_out_of_range_indices_are_ignored);
    RUN_TEST(test_reset_clears_everything);
    return UNITY_END();
}
