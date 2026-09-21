#include <unity.h>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "master.h"
#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "algorithm/switch/gate_switch.h"
#include "algorithm/switch/note_switch.h"
#include "algorithm/logic/flip_flop.h"
#include "algorithm/logic/counter.h"
#include "algorithm/logic/shift_register.h"
#include "algorithm/logic/edge.h"
#include "algorithm/modulator/gate_to_cv.h"

void setUp() {}
void tearDown() {}

// One master pass around a node: what was written is swapped in, the node
// runs, what it wrote is swapped out.
static void pass(BusManager& bus, Node& node, uint32_t now = 0) {
    bus.swap();
    node.process(bus, now);
    bus.swap();
}

// A pass with one gate bus set, then read.
static void gate_pass(BusManager& bus, Node& node, uint8_t in_bus, bool level, uint32_t now = 0) {
    bus.gate_write(in_bus, level);
    pass(bus, node, now);
}

// ---------------------------------------------------------------------------
// GateSwitch
// ---------------------------------------------------------------------------

// Parts on gate buses 0..n-1, select CV on CV 0, step on gate 10, reset on
// gate 11, out on gate 15.
static NodeConfig switch_config(uint8_t parts, bool cv_select) {
    NodeConfig c = node_config(ALGO_GATE_SWITCH);
    for (uint8_t i = 0; i < parts; i++) c.in_buses[i] = one_bus(i);
    if (cv_select) c.in_buses[GateSwitch::IN_SELECT] = one_bus(0);
    c.in_buses[GateSwitch::IN_STEP] = one_bus(10);
    c.in_buses[GateSwitch::IN_RESET] = one_bus(11);
    c.out_buses[0] = one_bus(15);
    return c;
}

static void test_gate_switch_steps_over_the_patched_parts_and_wraps() {
    BusManager bus;
    NodeConfig c = switch_config(2, false);
    GateSwitch node(c);
    TEST_ASSERT_EQUAL(1, node.get_param(GateSwitch::P_SELECT));

    // Part 1 high, part 2 low: out follows part 1.
    bus.gate_write(0, true);
    pass(bus, node);
    TEST_ASSERT_TRUE(bus.gate_read(15));

    // A step edge moves to part 2, which is low.
    bus.gate_write(0, true); bus.gate_write(10, true);
    pass(bus, node);
    TEST_ASSERT_FALSE(bus.gate_read(15));
    TEST_ASSERT_EQUAL(2, node.get_param(GateSwitch::P_SELECT));

    // Holding step high is not another edge.
    bus.gate_write(0, true); bus.gate_write(10, true);
    pass(bus, node);
    TEST_ASSERT_EQUAL(2, node.get_param(GateSwitch::P_SELECT));

    // Two parts patched, so the next edge wraps to part 1 rather than to
    // the three empty positions.
    bus.gate_write(10, false); pass(bus, node);
    bus.gate_write(0, true); bus.gate_write(10, true);
    pass(bus, node);
    TEST_ASSERT_EQUAL(1, node.get_param(GateSwitch::P_SELECT));
    TEST_ASSERT_TRUE(bus.gate_read(15));
}

static void test_gate_switch_reset_wins_and_an_empty_position_is_a_rest() {
    BusManager bus;
    NodeConfig c = switch_config(2, false);
    c.params[GateSwitch::P_STEPS] = 3;          // three positions, the third empty
    GateSwitch node(c);

    bus.gate_write(0, true); bus.gate_write(1, true);
    bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_EQUAL(2, node.position() + 1);
    bus.gate_write(0, true); bus.gate_write(1, true);
    bus.gate_write(10, false); pass(bus, node);
    bus.gate_write(0, true); bus.gate_write(1, true);
    bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_EQUAL(3, node.position() + 1);
    TEST_ASSERT_FALSE(bus.gate_read(15));       // nothing patched there: a rest

    // Reset and step in one pass: reset wins.
    bus.gate_write(10, false); pass(bus, node);
    bus.gate_write(0, true); bus.gate_write(10, true); bus.gate_write(11, true);
    pass(bus, node);
    TEST_ASSERT_EQUAL(1, node.position() + 1);
    TEST_ASSERT_TRUE(bus.gate_read(15));
}

static void test_gate_switch_select_level_addresses_a_part_per_count() {
    BusManager bus;
    NodeConfig c = switch_config(4, true);
    GateSwitch node(c);
    // Four parts patched: quarters of the range, and a step edge is ignored.
    const int16_t levels[4] = {0, CV_FULL / 4, CV_FULL / 2, (CV_FULL * 3) / 4};
    for (uint8_t k = 0; k < 4; k++) {
        bus.gate_write(k, true);
        bus.cv_write(0, levels[k]);
        bus.gate_write(10, (k & 1) != 0);
        pass(bus, node);
        TEST_ASSERT_EQUAL(k, node.position());
        TEST_ASSERT_TRUE(bus.gate_read(15));
    }
    // Past the top and below the bottom clamp rather than wrap.
    bus.cv_write(0, CV_MAX); pass(bus, node);
    TEST_ASSERT_EQUAL(3, node.position());
    bus.cv_write(0, -CV_HALF); pass(bus, node);
    TEST_ASSERT_EQUAL(0, node.position());
}

static void test_gate_switch_select_parameter_is_the_position() {
    BusManager bus;
    NodeConfig c = switch_config(3, false);
    GateSwitch node(c);
    TEST_ASSERT_TRUE(node.set_param(GateSwitch::P_SELECT, 3));
    TEST_ASSERT_FALSE(node.set_param(GateSwitch::P_SELECT, 6));
    bus.gate_write(2, true); pass(bus, node);
    TEST_ASSERT_TRUE(bus.gate_read(15));
    TEST_ASSERT_EQUAL(3, node.get_param(GateSwitch::P_SELECT));
    TEST_ASSERT_TRUE(node.set_param(GateSwitch::P_STEPS, 2));
    TEST_ASSERT_FALSE(node.set_param(GateSwitch::P_STEPS, 6));
    // Beyond the cycle it stays until the next step wraps it.
    bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_EQUAL(1, node.get_param(GateSwitch::P_SELECT));
}

// ---------------------------------------------------------------------------
// GateRouter
// ---------------------------------------------------------------------------

static void test_gate_router_sends_the_inlet_to_the_selected_outlet() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_GATE_ROUTER);
    c.in_buses[GateRouter::IN_SIGNAL] = one_bus(0);
    c.in_buses[GateRouter::IN_STEP] = one_bus(10);
    for (uint8_t k = 0; k < 3; k++) c.out_buses[k] = one_bus((uint8_t)(12 + k));
    GateRouter node(c);

    gate_pass(bus, node, 0, true);
    TEST_ASSERT_TRUE(bus.gate_read(12));
    TEST_ASSERT_FALSE(bus.gate_read(13));

    bus.gate_write(0, true); bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_FALSE(bus.gate_read(12));
    TEST_ASSERT_TRUE(bus.gate_read(13));

    // A held inlet makes it a decoder: one outlet high per position, and
    // three outlets patched is a cycle of three.
    bus.gate_write(0, true); bus.gate_write(10, false); pass(bus, node);
    bus.gate_write(0, true); bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_TRUE(bus.gate_read(14));
    bus.gate_write(0, true); bus.gate_write(10, false); pass(bus, node);
    bus.gate_write(0, true); bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_TRUE(bus.gate_read(12));
    TEST_ASSERT_EQUAL(1, node.get_param(GateRouter::P_SELECT));
}

// ---------------------------------------------------------------------------
// NoteSwitch / NoteRouter
// ---------------------------------------------------------------------------

static MidiEvent on(uint8_t note, uint8_t ch = 1) { return MidiEvent{MIDI_NOTE_ON, ch, note, 100}; }
static MidiEvent off(uint8_t note, uint8_t ch = 1) { return MidiEvent{MIDI_NOTE_OFF, ch, note, 0}; }

static void test_note_switch_releases_what_it_leaves_and_drops_the_old_note_off() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_NOTE_SWITCH);
    c.in_buses[0] = one_bus(0); c.in_buses[1] = one_bus(1);
    c.in_buses[NoteSwitch::IN_STEP] = one_bus(10);
    c.out_buses[0] = one_bus(5);
    NoteSwitch node(c);

    // Part 1 plays a note, part 2 plays another: only part 1 passes.
    bus.note_write(0, on(60)); bus.note_write(1, on(72));
    pass(bus, node);
    TEST_ASSERT_EQUAL(1, bus.note_count(5));
    TEST_ASSERT_EQUAL(60, bus.note_read(5, 0).data1);
    TEST_ASSERT_EQUAL(1, node.sounding_count());

    // Switch to part 2: the note-off for 60 goes out first, then part 2's
    // new note-on. Part 2's held 72 is not re-played.
    bus.note_write(1, on(74)); bus.gate_write(10, true);
    pass(bus, node);
    TEST_ASSERT_EQUAL(2, bus.note_count(5));
    TEST_ASSERT_TRUE(bus.note_read(5, 0).type == MIDI_NOTE_OFF && bus.note_read(5, 0).data1 == 60);
    TEST_ASSERT_TRUE(bus.note_read(5, 1).type == MIDI_NOTE_ON && bus.note_read(5, 1).data1 == 74);

    // Part 1's own note-off, arriving now, is not forwarded: it was released
    // already, and part 1 is not on the bus.
    bus.note_write(0, off(60)); bus.gate_write(10, true);
    pass(bus, node);
    TEST_ASSERT_EQUAL(0, bus.note_count(5));

    // A CC on the selected part passes; one on the other does not.
    bus.note_write(1, MidiEvent{MIDI_CONTROL_CHANGE, 1, 1, 64});
    bus.note_write(0, MidiEvent{MIDI_CONTROL_CHANGE, 1, 1, 0});
    bus.gate_write(10, true); pass(bus, node);
    TEST_ASSERT_EQUAL(1, bus.note_count(5));
    TEST_ASSERT_EQUAL(64, bus.note_read(5, 0).data2);

    // The parameter is a switch too, and releases on the next pass.
    TEST_ASSERT_EQUAL(1, node.sounding_count());
    TEST_ASSERT_TRUE(node.set_param(NoteSwitch::P_SELECT, 1));
    pass(bus, node);
    TEST_ASSERT_EQUAL(1, bus.note_count(5));
    TEST_ASSERT_TRUE(bus.note_read(5, 0).type == MIDI_NOTE_OFF && bus.note_read(5, 0).data1 == 74);
    TEST_ASSERT_EQUAL(0, node.sounding_count());

    // Silence releases everything for a handover.
    bus.note_write(0, on(48)); pass(bus, node);
    node.silence(bus); bus.swap();
    TEST_ASSERT_EQUAL(1, bus.note_count(5));
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, bus.note_read(5, 0).type);
}

static void test_note_router_releases_on_the_outlet_it_leaves() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_NOTE_ROUTER);
    c.in_buses[NoteRouter::IN_SIGNAL] = one_bus(0);
    c.in_buses[NoteRouter::IN_SELECT] = one_bus(0);      // CV 0 addresses
    c.out_buses[0] = one_bus(4); c.out_buses[1] = one_bus(5);
    NoteRouter node(c);

    bus.cv_write(0, 0); bus.note_write(0, on(60)); pass(bus, node);
    TEST_ASSERT_EQUAL(1, bus.note_count(4));
    TEST_ASSERT_EQUAL(0, bus.note_count(5));

    // Address outlet 2: 60 is released on outlet 1, and the new note lands
    // on outlet 2.
    bus.cv_write(0, CV_HALF); bus.note_write(0, on(62)); pass(bus, node);
    TEST_ASSERT_EQUAL(1, bus.note_count(4));
    TEST_ASSERT_TRUE(bus.note_read(4, 0).type == MIDI_NOTE_OFF && bus.note_read(4, 0).data1 == 60);
    TEST_ASSERT_EQUAL(1, bus.note_count(5));
    TEST_ASSERT_EQUAL(62, bus.note_read(5, 0).data1);

    // Its note-off follows it to outlet 2.
    bus.cv_write(0, CV_HALF); bus.note_write(0, off(62)); pass(bus, node);
    TEST_ASSERT_EQUAL(0, bus.note_count(4));
    TEST_ASSERT_EQUAL(1, bus.note_count(5));
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, bus.note_read(5, 0).type);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// ---------------------------------------------------------------------------
// FlipFlop
// ---------------------------------------------------------------------------

// data on gate 0, K on gate 1, clock on gate 2, clear on gate 3; Q on 8,
// not Q on 9.
static NodeConfig ff_config(uint8_t type, bool with_data = true, bool with_clock = true) {
    NodeConfig c = node_config(ALGO_FLIP_FLOP);
    if (with_data) c.in_buses[FlipFlop::IN_DATA] = one_bus(0);
    c.in_buses[FlipFlop::IN_K] = one_bus(1);
    if (with_clock) c.in_buses[FlipFlop::IN_CLOCK] = one_bus(2);
    c.in_buses[FlipFlop::IN_CLEAR] = one_bus(3);
    c.out_buses[0] = one_bus(8); c.out_buses[1] = one_bus(9);
    c.params[FlipFlop::P_TYPE] = type;
    return c;
}

static void ff_pass(BusManager& bus, FlipFlop& node, bool d, bool k, bool clock, bool clear = false) {
    bus.gate_write(0, d); bus.gate_write(1, k); bus.gate_write(2, clock); bus.gate_write(3, clear);
    pass(bus, node);
}

static void test_d_flip_flop_takes_data_on_the_edge_only() {
    BusManager bus;
    NodeConfig c = ff_config(FlipFlop::FF_D);
    FlipFlop node(c);
    ff_pass(bus, node, true, false, false);
    TEST_ASSERT_FALSE(node.q());                 // data alone does nothing
    TEST_ASSERT_TRUE(bus.gate_read(9));          // not Q is the complement
    ff_pass(bus, node, true, false, true);
    TEST_ASSERT_TRUE(node.q());
    TEST_ASSERT_TRUE(bus.gate_read(8));
    TEST_ASSERT_FALSE(bus.gate_read(9));
    ff_pass(bus, node, false, false, true);      // still high: no edge
    TEST_ASSERT_TRUE(node.q());
    ff_pass(bus, node, false, false, false);
    ff_pass(bus, node, false, false, true);
    TEST_ASSERT_FALSE(node.q());
    // Clear drops it whatever the clock does, and wins over the edge.
    ff_pass(bus, node, true, false, false);
    ff_pass(bus, node, true, false, true, true);
    TEST_ASSERT_FALSE(node.q());
}

static void test_d_latch_is_transparent_while_the_clock_is_open() {
    BusManager bus;
    NodeConfig c = ff_config(FlipFlop::FF_D_LATCH);
    FlipFlop node(c);
    ff_pass(bus, node, true, false, true);
    TEST_ASSERT_TRUE(node.q());
    ff_pass(bus, node, false, false, true);      // still open: follows
    TEST_ASSERT_FALSE(node.q());
    ff_pass(bus, node, true, false, true);
    ff_pass(bus, node, false, false, false);     // closed: holds
    TEST_ASSERT_TRUE(node.q());
}

static void test_t_flip_flop_with_nothing_on_data_divides_by_two() {
    BusManager bus;
    NodeConfig c = ff_config(FlipFlop::FF_T, false);
    FlipFlop node(c);
    bool expect = false;
    for (int i = 0; i < 6; i++) {
        ff_pass(bus, node, false, false, true);
        expect = !expect;
        TEST_ASSERT_EQUAL(expect, node.q());
        ff_pass(bus, node, false, false, false);
        TEST_ASSERT_EQUAL(expect, node.q());
    }
    // With data patched, it only toggles while data is high.
    NodeConfig c2 = ff_config(FlipFlop::FF_T, true);
    FlipFlop gated(c2);
    ff_pass(bus, gated, false, false, true);
    TEST_ASSERT_FALSE(gated.q());
    ff_pass(bus, gated, false, false, false);
    ff_pass(bus, gated, true, false, true);
    TEST_ASSERT_TRUE(gated.q());
}

static void test_jk_flip_flop_truth_table() {
    BusManager bus;
    NodeConfig c = ff_config(FlipFlop::FF_JK);
    FlipFlop node(c);
    auto clock = [&](bool j, bool k) { ff_pass(bus, node, j, k, false); ff_pass(bus, node, j, k, true); };
    clock(false, false); TEST_ASSERT_FALSE(node.q());   // hold
    clock(true, false);  TEST_ASSERT_TRUE(node.q());    // set
    clock(false, false); TEST_ASSERT_TRUE(node.q());    // hold
    clock(false, true);  TEST_ASSERT_FALSE(node.q());   // reset
    clock(true, true);   TEST_ASSERT_TRUE(node.q());    // toggle
    clock(true, true);   TEST_ASSERT_FALSE(node.q());
}

static void test_sr_flip_flop_by_level_without_a_clock_and_reset_wins() {
    BusManager bus;
    NodeConfig c = ff_config(FlipFlop::FF_SR, true, false);
    FlipFlop node(c);
    ff_pass(bus, node, true, false, false);
    TEST_ASSERT_TRUE(node.q());
    ff_pass(bus, node, false, false, false);     // holds
    TEST_ASSERT_TRUE(node.q());
    ff_pass(bus, node, true, true, false);       // both: reset wins
    TEST_ASSERT_FALSE(node.q());

    // With a clock, S and R are read on the edge only.
    NodeConfig c2 = ff_config(FlipFlop::FF_SR);
    FlipFlop clocked(c2);
    ff_pass(bus, clocked, true, false, false);
    TEST_ASSERT_FALSE(clocked.q());
    ff_pass(bus, clocked, true, false, true);
    TEST_ASSERT_TRUE(clocked.q());
}

static void test_flip_flop_falling_edge_and_type_change_keeps_the_bit() {
    BusManager bus;
    NodeConfig c = ff_config(FlipFlop::FF_D);
    c.params[FlipFlop::P_EDGE] = FlipFlop::EDGE_FALLING;
    FlipFlop node(c);
    ff_pass(bus, node, true, false, true);       // rising: nothing
    TEST_ASSERT_FALSE(node.q());
    ff_pass(bus, node, true, false, false);      // falling: takes data
    TEST_ASSERT_TRUE(node.q());
    TEST_ASSERT_TRUE(node.set_param(FlipFlop::P_TYPE, FlipFlop::FF_T));
    TEST_ASSERT_TRUE(node.q());
    TEST_ASSERT_EQUAL(FlipFlop::FF_T, node.get_param(FlipFlop::P_TYPE));
    TEST_ASSERT_FALSE(node.set_param(FlipFlop::P_TYPE, 9));
}

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------

// clock on gate 0, reset on gate 1; carry on gate 8, count on CV 0, bits on
// gates 9..13.
static NodeConfig counter_config(uint8_t length, uint8_t direction = 0) {
    NodeConfig c = node_config(ALGO_COUNTER);
    c.in_buses[0] = one_bus(0); c.in_buses[1] = one_bus(1);
    c.out_buses[Counter::OUT_CARRY] = one_bus(8);
    c.out_buses[Counter::OUT_COUNT] = one_bus(0);
    for (uint8_t b = 0; b < Counter::BITS; b++) c.out_buses[Counter::OUT_BIT + b] = one_bus((uint8_t)(9 + b));
    c.params[Counter::P_LENGTH] = length;
    c.params[Counter::P_DIRECTION] = direction;
    return c;
}

static void clock_counter(BusManager& bus, Counter& node, uint32_t& now, bool reset = false) {
    bus.gate_write(0, true); bus.gate_write(1, reset);
    pass(bus, node, now); now += 10000;
    bus.gate_write(0, false);
    pass(bus, node, now); now += 10000;
}

static void test_counter_carries_on_the_wrap_and_not_on_the_first_step_after_reset() {
    BusManager bus;
    NodeConfig c = counter_config(4);
    Counter node(c);
    uint32_t now = 0;
    // Before the first edge nothing is written.
    pass(bus, node, now);
    TEST_ASSERT_EQUAL(0, bus.cv_read(0));

    // Reset with the first edge: step 0, no carry.
    bus.gate_write(0, true); bus.gate_write(1, true);
    pass(bus, node, now);
    TEST_ASSERT_EQUAL(0, node.position());
    TEST_ASSERT_FALSE(bus.gate_read(8));
    TEST_ASSERT_EQUAL(0, node.carries());
    bus.gate_write(0, false); pass(bus, node, now); now += 10000;

    // 1, 2, 3: the count as quarters of the range, the bits as binary.
    const int16_t levels[4] = {0, CV_FULL / 4, CV_FULL / 2, (CV_FULL * 3) / 4};
    for (uint8_t k = 1; k < 4; k++) {
        bus.gate_write(0, true); pass(bus, node, now);
        TEST_ASSERT_EQUAL(k, node.position());
        TEST_ASSERT_EQUAL(levels[k], bus.cv_read(0));
        TEST_ASSERT_EQUAL((k & 1) != 0, bus.gate_read(9));
        TEST_ASSERT_EQUAL((k & 2) != 0, bus.gate_read(10));
        TEST_ASSERT_FALSE(bus.gate_read(8));
        now += 10000;
        bus.gate_write(0, false); pass(bus, node, now); now += 10000;
    }
    // The wrap: back to 0 and the carry fires, for the trigger width.
    bus.gate_write(0, true); pass(bus, node, now);
    TEST_ASSERT_EQUAL(0, node.position());
    TEST_ASSERT_TRUE(bus.gate_read(8));
    TEST_ASSERT_EQUAL(1, node.carries());
    now += TRIGGER_WIDTH_US + 1000;
    bus.gate_write(0, false); pass(bus, node, now);
    TEST_ASSERT_FALSE(bus.gate_read(8));
}

static void test_counter_reverse_carries_on_the_last_step_and_length_one_on_every_edge() {
    BusManager bus;
    NodeConfig c = counter_config(3, StepEngine::SEQ_REVERSE);
    Counter node(c);
    uint32_t now = 0;
    clock_counter(bus, node, now, true);        // first step going backwards is 2
    TEST_ASSERT_EQUAL(2, node.position());
    TEST_ASSERT_EQUAL(0, node.carries());
    clock_counter(bus, node, now);              // 1
    clock_counter(bus, node, now);              // 0
    TEST_ASSERT_EQUAL(0, node.carries());
    clock_counter(bus, node, now);              // wraps to 2
    TEST_ASSERT_EQUAL(2, node.position());
    TEST_ASSERT_EQUAL(1, node.carries());

    NodeConfig c1 = counter_config(1);
    Counter one(c1);
    clock_counter(bus, one, now, true);
    TEST_ASSERT_EQUAL(0, one.carries());
    clock_counter(bus, one, now);
    clock_counter(bus, one, now);
    TEST_ASSERT_EQUAL(2, one.carries());
}

static void test_counter_length_and_direction_are_live() {
    BusManager bus;
    NodeConfig c = counter_config(8);
    Counter node(c);
    uint32_t now = 0;
    clock_counter(bus, node, now, true);
    for (int i = 0; i < 5; i++) clock_counter(bus, node, now);
    TEST_ASSERT_EQUAL(5, node.position());
    TEST_ASSERT_TRUE(node.set_param(Counter::P_LENGTH, 4));
    TEST_ASSERT_EQUAL(4, node.get_param(Counter::P_LENGTH));
    TEST_ASSERT_EQUAL(5, node.position());       // clamped on the next edge, not now
    clock_counter(bus, node, now);
    TEST_ASSERT_TRUE(node.position() < 4);
    TEST_ASSERT_FALSE(node.set_param(Counter::P_LENGTH, 0));
    TEST_ASSERT_TRUE(node.set_param(Counter::P_DIRECTION, StepEngine::SEQ_PINGPONG));
    TEST_ASSERT_FALSE(node.set_param(Counter::P_DIRECTION, StepEngine::SEQ_DIRECTIONS));
    TEST_ASSERT_EQUAL(TRIGGER_WIDTH_US / 1000, node.get_param(Counter::P_WIDTH));
}

// ---------------------------------------------------------------------------
// ShiftRegister
// ---------------------------------------------------------------------------

// clock on gate 0, data on gate 1, clear on gate 2; taps on gates 8..15.
static NodeConfig register_config(uint8_t length = 0, bool loop = false) {
    NodeConfig c = node_config(ALGO_SHIFT_REGISTER);
    c.in_buses[ShiftRegister::IN_CLOCK] = one_bus(0);
    c.in_buses[ShiftRegister::IN_DATA] = one_bus(1);
    c.in_buses[ShiftRegister::IN_CLEAR] = one_bus(2);
    for (uint8_t t = 0; t < ShiftRegister::TAPS; t++) c.out_buses[t] = one_bus((uint8_t)(8 + t));
    c.params[ShiftRegister::P_LENGTH] = length;
    c.params[ShiftRegister::P_LOOP] = loop ? 1 : 0;
    return c;
}

static void shift(BusManager& bus, ShiftRegister& node, bool data, bool clear = false) {
    bus.gate_write(0, true); bus.gate_write(1, data); bus.gate_write(2, clear);
    pass(bus, node);
    bus.gate_write(0, false); bus.gate_write(1, data);
    pass(bus, node);
}

static void test_shift_register_delays_a_gate_by_one_clock_per_tap() {
    BusManager bus;
    NodeConfig c = register_config();
    ShiftRegister node(c);
    shift(bus, node, true);
    TEST_ASSERT_EQUAL(0x01, node.bits());
    TEST_ASSERT_TRUE(bus.gate_read(8));
    shift(bus, node, false);
    shift(bus, node, true);
    TEST_ASSERT_EQUAL(0x05, node.bits());
    TEST_ASSERT_TRUE(bus.gate_read(8));
    TEST_ASSERT_FALSE(bus.gate_read(9));
    TEST_ASSERT_TRUE(bus.gate_read(10));
    // Eight more clocks and the first gate has fallen off the end.
    for (int i = 0; i < 8; i++) shift(bus, node, false);
    TEST_ASSERT_EQUAL(0, node.bits());
    // Clear empties it, and wins over the shift in the same pass.
    shift(bus, node, true);
    shift(bus, node, true, true);
    TEST_ASSERT_EQUAL(0, node.bits());
}

static void test_shift_register_loop_keeps_what_was_played_in() {
    BusManager bus;
    NodeConfig c = register_config(4, true);
    ShiftRegister node(c);
    shift(bus, node, true);
    shift(bus, node, false);
    shift(bus, node, false);
    shift(bus, node, false);
    TEST_ASSERT_EQUAL(0x08, node.bits());        // at tap 4, the loop's end
    shift(bus, node, false);                     // comes back round
    TEST_ASSERT_EQUAL(0x11, node.bits());
    shift(bus, node, true);                      // another gate joins it
    TEST_ASSERT_EQUAL(0x23, node.bits());
    TEST_ASSERT_TRUE(node.set_param(ShiftRegister::P_LOOP, 0));
    for (int i = 0; i < 8; i++) shift(bus, node, false);
    TEST_ASSERT_EQUAL(0, node.bits());
    TEST_ASSERT_FALSE(node.set_param(ShiftRegister::P_LENGTH, 9));
    TEST_ASSERT_EQUAL(4, node.get_param(ShiftRegister::P_LENGTH));
}

// ---------------------------------------------------------------------------
// Edge
// ---------------------------------------------------------------------------

static void test_edge_fires_rise_and_fall_as_triggers() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_EDGE);
    c.in_buses[0] = one_bus(0);
    c.out_buses[Edge::OUT_RISE] = one_bus(8);
    c.out_buses[Edge::OUT_FALL] = one_bus(9);
    c.params[Edge::P_WIDTH] = 2;
    Edge node(c);
    uint32_t now = 0;
    // A gate already up on the first pass is not an edge.
    gate_pass(bus, node, 0, true, now);
    TEST_ASSERT_FALSE(bus.gate_read(8));
    gate_pass(bus, node, 0, false, now);
    TEST_ASSERT_TRUE(bus.gate_read(9));
    now += 1000;
    gate_pass(bus, node, 0, false, now);
    TEST_ASSERT_TRUE(bus.gate_read(9));          // still inside 2 ms
    now += 1500;
    gate_pass(bus, node, 0, false, now);
    TEST_ASSERT_FALSE(bus.gate_read(9));
    gate_pass(bus, node, 0, true, now);
    TEST_ASSERT_TRUE(bus.gate_read(8));
    TEST_ASSERT_FALSE(bus.gate_read(9));
    TEST_ASSERT_EQUAL(2, node.get_param(Edge::P_WIDTH));
    TEST_ASSERT_FALSE(node.set_param(Edge::P_WIDTH, 0));
}

// ---------------------------------------------------------------------------
// GateToCv
// ---------------------------------------------------------------------------

static void test_gate_to_cv_is_two_levels() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_GATE_TO_CV);
    c.in_buses[0] = one_bus(0);
    c.out_buses[0] = one_bus(0);
    GateToCv node(c);
    gate_pass(bus, node, 0, false);
    TEST_ASSERT_EQUAL(0, bus.cv_read(0));
    gate_pass(bus, node, 0, true);
    TEST_ASSERT_EQUAL(CV_MAX, bus.cv_read(0));
    TEST_ASSERT_TRUE(node.set_param(GateToCv::P_LOW, 50));
    TEST_ASSERT_TRUE(node.set_param(GateToCv::P_HIGH, 25));
    gate_pass(bus, node, 0, false);
    TEST_ASSERT_EQUAL(CV_MAX / 2, bus.cv_read(0));
    gate_pass(bus, node, 0, true);
    TEST_ASSERT_EQUAL(CV_MAX / 4, bus.cv_read(0));
    TEST_ASSERT_FALSE(node.set_param(GateToCv::P_HIGH, 0));
    TEST_ASSERT_FALSE(node.set_param(GateToCv::P_LOW, 101));
}

// ---------------------------------------------------------------------------
// Through the master: a counter's count addresses a switch in the same pass
// ---------------------------------------------------------------------------

// Jack 1 clocks a two-step counter whose count outlet addresses a GateSwitch
// between jack 2 (part 1) and jack 3 (part 2); jack 4 is the switch's out.
// The scheduler orders the counter before the switch because it writes the
// CV bus the switch reads, so the part changes on the very pass the count
// does.
static void test_counter_addresses_a_switch_in_one_pass() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, one_bus(0)};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_IN, one_bus(1)};
    p.gate_ports[2] = GatePortConfig{GATE_PORT_IN, one_bus(2)};
    p.gate_ports[3] = GatePortConfig{GATE_PORT_OUT, one_bus(3)};
    // The switch first in patch order, so the schedule has to reorder it.
    p.nodes[0] = node_config(ALGO_GATE_SWITCH);
    p.nodes[0].in_buses[0] = one_bus(1);
    p.nodes[0].in_buses[1] = one_bus(2);
    p.nodes[0].in_buses[GateSwitch::IN_SELECT] = one_bus(0);
    p.nodes[0].out_buses[0] = one_bus(3);
    p.nodes[1] = node_config(ALGO_COUNTER);
    p.nodes[1].in_buses[0] = one_bus(0);
    p.nodes[1].out_buses[Counter::OUT_COUNT] = one_bus(0);
    p.nodes[1].params[Counter::P_LENGTH] = 2;
    p.n_nodes = 2;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    gpio.set_input(1, GPIO_HIGH);             // part 1 high
    gpio.set_input(2, GPIO_LOW);              // part 2 low
    for (int i = 0; i < 3; i++) { master.pass(now); now += 1000; }
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[3]);

    // One clock: the counter goes to step 0 (its first), count 0: still part 1.
    gpio.set_input(0, GPIO_HIGH); master.pass(now); now += 1000;
    gpio.set_input(0, GPIO_LOW);  master.pass(now); now += 1000;
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[3]);
    // The next clock: count half scale, so part 2, on the pass the edge is
    // sampled - and the jack shows it one pass later, as every output does.
    gpio.set_input(0, GPIO_HIGH); master.pass(now); now += 1000;
    master.pass(now); now += 1000;
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[3]);
    gpio.set_input(2, GPIO_HIGH);
    master.pass(now); now += 1000; master.pass(now); now += 1000;
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[3]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_gate_switch_steps_over_the_patched_parts_and_wraps);
    RUN_TEST(test_gate_switch_reset_wins_and_an_empty_position_is_a_rest);
    RUN_TEST(test_gate_switch_select_level_addresses_a_part_per_count);
    RUN_TEST(test_gate_switch_select_parameter_is_the_position);
    RUN_TEST(test_gate_router_sends_the_inlet_to_the_selected_outlet);
    RUN_TEST(test_note_switch_releases_what_it_leaves_and_drops_the_old_note_off);
    RUN_TEST(test_note_router_releases_on_the_outlet_it_leaves);
    RUN_TEST(test_d_flip_flop_takes_data_on_the_edge_only);
    RUN_TEST(test_d_latch_is_transparent_while_the_clock_is_open);
    RUN_TEST(test_t_flip_flop_with_nothing_on_data_divides_by_two);
    RUN_TEST(test_jk_flip_flop_truth_table);
    RUN_TEST(test_sr_flip_flop_by_level_without_a_clock_and_reset_wins);
    RUN_TEST(test_flip_flop_falling_edge_and_type_change_keeps_the_bit);
    RUN_TEST(test_counter_carries_on_the_wrap_and_not_on_the_first_step_after_reset);
    RUN_TEST(test_counter_reverse_carries_on_the_last_step_and_length_one_on_every_edge);
    RUN_TEST(test_counter_length_and_direction_are_live);
    RUN_TEST(test_shift_register_delays_a_gate_by_one_clock_per_tap);
    RUN_TEST(test_shift_register_loop_keeps_what_was_played_in);
    RUN_TEST(test_edge_fires_rise_and_fall_as_triggers);
    RUN_TEST(test_gate_to_cv_is_two_levels);
    RUN_TEST(test_counter_addresses_a_switch_in_one_pass);
    return UNITY_END();
}
