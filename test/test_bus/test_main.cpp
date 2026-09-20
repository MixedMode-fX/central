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

// --- a port's set of buses ------------------------------------------------
//
// An inlet reading two buses is a merge under the domain's own fan-in rule,
// and it costs the two writers nothing: each keeps its own bus.

static BusSet both(uint8_t a, uint8_t b) { BusSet s{}; s.add(a); s.add(b); return s; }

static void test_reading_a_set_of_gate_buses_is_their_or() {
    BusManager bus;
    bus.gate_write(1, true);
    bus.swap();
    TEST_ASSERT_TRUE(bus.gate_read(both(1, 5)));
    TEST_ASSERT_FALSE(bus.gate_read(one_bus(5)));      // the other writer is untouched
    bus.swap();
    TEST_ASSERT_FALSE(bus.gate_read(both(1, 5)));
}

static void test_writing_a_set_of_gate_buses_writes_every_one() {
    BusManager bus;
    bus.gate_write(both(2, 7), true);
    bus.swap();
    TEST_ASSERT_TRUE(bus.gate_read(2));
    TEST_ASSERT_TRUE(bus.gate_read(7));
    TEST_ASSERT_FALSE(bus.gate_read(3));
}

static void test_reading_a_set_of_cv_buses_is_their_sum() {
    BusManager bus;
    bus.cv_write(0, 300);
    bus.cv_write(1, 400);
    bus.swap();
    TEST_ASSERT_EQUAL(700, bus.cv_read(both(0, 1)));
    TEST_ASSERT_EQUAL(300, bus.cv_read(one_bus(0)));
}

static void test_reading_a_set_of_note_buses_concatenates_in_bus_order() {
    BusManager bus;
    bus.note_write(4, note_on(64));
    bus.note_write(1, note_on(60));
    bus.note_write(1, note_on(62));
    bus.swap();
    const BusSet set = both(1, 4);
    TEST_ASSERT_EQUAL(3, bus.note_count(set));
    // The lower bus first, arrival order within it - so a merge is the same
    // every pass and the tests can say what a node saw.
    TEST_ASSERT_EQUAL(60, bus.note_read(set, 0).data1);
    TEST_ASSERT_EQUAL(62, bus.note_read(set, 1).data1);
    TEST_ASSERT_EQUAL(64, bus.note_read(set, 2).data1);
    TEST_ASSERT_EQUAL(0, bus.note_read(set, 3).type);      // past the end
}

static void test_a_note_written_to_a_set_reaches_every_bus_and_room_is_the_tightest() {
    BusManager bus;
    for (uint8_t i = 0; i < NOTE_QUEUE_DEPTH - 1u; i++) bus.note_write(6, note_on(60));
    const BusSet set = both(3, 6);
    TEST_ASSERT_EQUAL(1, bus.note_room(set));              // bus 6 is nearly full
    TEST_ASSERT_TRUE(bus.note_write(set, note_on(72)));
    bus.swap();
    TEST_ASSERT_EQUAL(1, bus.note_count(one_bus(3)));
    TEST_ASSERT_EQUAL(NOTE_QUEUE_DEPTH, bus.note_count(one_bus(6)));
}

static void test_an_empty_set_reads_nothing_and_writes_nowhere() {
    BusManager bus;
    bus.gate_write(BusSet{}, true);
    bus.cv_write(BusSet{}, 1000);
    bus.note_write(BusSet{}, note_on(60));
    bus.swap();
    TEST_ASSERT_FALSE(bus.gate_read(BusSet{}));
    TEST_ASSERT_EQUAL(0, bus.cv_read(BusSet{}));
    TEST_ASSERT_EQUAL(0, bus.note_count(BusSet{}));
    for (uint8_t b = 0; b < N_GATE_BUS; b++) TEST_ASSERT_FALSE(bus.gate_read(b));
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
    RUN_TEST(test_reading_a_set_of_gate_buses_is_their_or);
    RUN_TEST(test_writing_a_set_of_gate_buses_writes_every_one);
    RUN_TEST(test_reading_a_set_of_cv_buses_is_their_sum);
    RUN_TEST(test_reading_a_set_of_note_buses_concatenates_in_bus_order);
    RUN_TEST(test_a_note_written_to_a_set_reaches_every_bus_and_room_is_the_tightest);
    RUN_TEST(test_an_empty_set_reads_nothing_and_writes_nowhere);
    return UNITY_END();
}
