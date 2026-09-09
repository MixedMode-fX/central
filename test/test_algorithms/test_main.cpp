#include <unity.h>
#include <stdio.h>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "algorithm/logic/gates.h"
#include "algorithm/switch/sustain.h"
#include "algorithm/midi/gate_to_note.h"
#include "algorithm/midi/transpose.h"
#include "algorithm/midi/arpeggiator.h"

void setUp() {}
void tearDown() {}

// Node-level tests: each node is driven directly through a BusManager.
// Inputs are written, swapped in, the node runs, and the result is swapped
// out - exactly what one master pass does around the node.

static NodeConfig gate_config(uint8_t id, uint8_t n_in, uint8_t out_bus) {
    NodeConfig c = node_config(id);
    for (uint8_t i = 0; i < n_in; i++) c.in_bus[i] = i;   // inputs on gate buses 0..n-1
    c.out_bus[0] = out_bus;
    return c;
}

template <class T>
static bool run_gate(const uint8_t* inputs, uint8_t n_in, uint8_t id, uint8_t out_bus) {
    BusManager bus;
    NodeConfig c = gate_config(id, n_in, out_bus);
    T node(c);
    for (uint8_t i = 0; i < n_in; i++) bus.gate_write(i, inputs[i] != 0);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    return bus.gate_read(out_bus);
}

template <class T>
static void check_truth_table(uint8_t id, const uint8_t expected[4], const char* name) {
    for (uint8_t combo = 0; combo < 4; combo++) {
        const uint8_t inputs[2] = {(uint8_t)(combo & 1), (uint8_t)((combo >> 1) & 1)};
        char msg[48];
        snprintf(msg, sizeof msg, "%s a=%u b=%u", name, inputs[0], inputs[1]);
        TEST_ASSERT_EQUAL_MESSAGE(expected[combo] != 0, run_gate<T>(inputs, 2, id, 5), msg);
    }
}

// ---------------------------------------------------------------------------
// Logic gates: seven gates x four two-input combinations (a,b) = 00 10 01 11
// ---------------------------------------------------------------------------

static void test_and_truth_table()  { const uint8_t e[4] = {0, 0, 0, 1}; check_truth_table<LogicAND >(ALGO_LOGIC_AND,  e, "AND"); }
static void test_nand_truth_table() { const uint8_t e[4] = {1, 1, 1, 0}; check_truth_table<LogicNAND>(ALGO_LOGIC_NAND, e, "NAND"); }
static void test_or_truth_table()   { const uint8_t e[4] = {0, 1, 1, 1}; check_truth_table<LogicOR  >(ALGO_LOGIC_OR,   e, "OR"); }
static void test_nor_truth_table()  { const uint8_t e[4] = {1, 0, 0, 0}; check_truth_table<LogicNOR >(ALGO_LOGIC_NOR,  e, "NOR"); }
static void test_xor_truth_table()  { const uint8_t e[4] = {0, 1, 1, 0}; check_truth_table<LogicXOR >(ALGO_LOGIC_XOR,  e, "XOR"); }
static void test_xnor_truth_table() { const uint8_t e[4] = {1, 0, 0, 1}; check_truth_table<LogicXNOR>(ALGO_LOGIC_XNOR, e, "XNOR"); }

static void test_not_truth_table() {
    const uint8_t low = 0, high = 1;
    TEST_ASSERT_TRUE(run_gate<LogicNot>(&low, 1, ALGO_LOGIC_NOT, 5));
    TEST_ASSERT_FALSE(run_gate<LogicNot>(&high, 1, ALGO_LOGIC_NOT, 5));
}

// XOR over more than two inputs is parity (documented decision).
static void test_xor_three_inputs_is_parity() {
    for (uint8_t combo = 0; combo < 8; combo++) {
        uint8_t inputs[3], ones = 0;
        for (uint8_t i = 0; i < 3; i++) { inputs[i] = (combo >> i) & 1; ones += inputs[i]; }
        TEST_ASSERT_EQUAL((ones & 1) != 0, run_gate<LogicXOR>(inputs, 3, ALGO_LOGIC_XOR, 5));
    }
}

// An unconnected inlet is skipped, so it cannot force an OR high or an AND low.
static void test_unconnected_inlets_are_skipped() {
    const uint8_t high = 1, low = 0;
    TEST_ASSERT_TRUE(run_gate<LogicOR>(&high, 1, ALGO_LOGIC_OR, 5));
    TEST_ASSERT_FALSE(run_gate<LogicOR>(&low, 1, ALGO_LOGIC_OR, 5));
    TEST_ASSERT_TRUE(run_gate<LogicAND>(&high, 1, ALGO_LOGIC_AND, 5));
}

// ---------------------------------------------------------------------------
// Sustain
// ---------------------------------------------------------------------------

static uint8_t sustain_messages(BusManager& bus, uint8_t note_bus) { return bus.note_count(note_bus); }

// Bounce faster than the debounce interval yields exactly one CC64.
static void test_sustain_debounce_yields_exactly_one_cc() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_SUSTAIN);
    c.in_bus[0] = 0; c.out_bus[0] = 0;
    Sustain node(c);

    uint32_t now = 0;
    uint16_t sent = 0;
    // Settle "up" first: the first stable level is reported (initial state).
    for (int i = 0; i < 10; i++) { bus.gate_write(0, false); bus.swap(); node.process(bus, now); bus.swap(); sent += sustain_messages(bus, 0); now += 1000; }
    TEST_ASSERT_EQUAL(1, sent);
    TEST_ASSERT_EQUAL(0, bus.note_read(0, 0).data2);

    sent = 0;
    // Bounce: toggle every 1 ms for 20 ms.
    for (int i = 0; i < 20; i++) { bus.gate_write(0, (i & 1) != 0); bus.swap(); node.process(bus, now); bus.swap(); sent += sustain_messages(bus, 0); now += 1000; }
    TEST_ASSERT_EQUAL(0, sent);
    // Settle pressed for 10 ms.
    MidiEvent last = {0, 0, 0, 0};
    for (int i = 0; i < 10; i++) { bus.gate_write(0, true); bus.swap(); node.process(bus, now); bus.swap(); if (bus.note_count(0)) { sent += bus.note_count(0); last = bus.note_read(0, 0); } now += 1000; }
    TEST_ASSERT_EQUAL(1, sent);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, last.type);
    TEST_ASSERT_EQUAL(1, last.channel);
    TEST_ASSERT_EQUAL(64, last.data1);
    TEST_ASSERT_EQUAL(127, last.data2);
    TEST_ASSERT_EQUAL(1, node.pedal_down());
}

static void test_sustain_params_channel_controller_invert() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_SUSTAIN);
    c.in_bus[0] = 0; c.out_bus[0] = 3;
    c.params[0] = 5; c.params[1] = 66; c.params[2] = 1;
    Sustain node(c);
    uint32_t now = 0;
    MidiEvent last = {0, 0, 0, 0};
    uint8_t sent = 0;
    for (int i = 0; i < 10; i++) { bus.gate_write(0, false); bus.swap(); node.process(bus, now); bus.swap(); if (bus.note_count(3)) { sent++; last = bus.note_read(3, 0); } now += 1000; }
    TEST_ASSERT_EQUAL(1, sent);
    TEST_ASSERT_EQUAL(5, last.channel);
    TEST_ASSERT_EQUAL(66, last.data1);
    TEST_ASSERT_EQUAL(127, last.data2);   // inverted: open contact = pedal down
}

// ---------------------------------------------------------------------------
// GateToNote, Transpose, Arpeggiator
// ---------------------------------------------------------------------------

static void test_gate_to_note_edges() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_GATE_TO_NOTE);
    c.in_bus[0] = 1; c.out_bus[0] = 2; c.params[0] = 48; c.params[1] = 90; c.params[2] = 3;
    GateToNote node(c);
    bus.gate_write(1, true); bus.swap(); node.process(bus, 0); bus.swap();
    TEST_ASSERT_EQUAL(1, bus.note_count(2));
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, bus.note_read(2, 0).type);
    TEST_ASSERT_EQUAL(48, bus.note_read(2, 0).data1);
    TEST_ASSERT_EQUAL(90, bus.note_read(2, 0).data2);
    TEST_ASSERT_EQUAL(3, bus.note_read(2, 0).channel);
    bus.gate_write(1, true); bus.swap(); node.process(bus, 0); bus.swap();
    TEST_ASSERT_EQUAL(0, bus.note_count(2));                    // held: nothing
    bus.swap(); node.process(bus, 0); bus.swap();               // low
    TEST_ASSERT_EQUAL(1, bus.note_count(2));
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, bus.note_read(2, 0).type);
}

static void test_transpose_shifts_notes_and_passes_the_rest() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_TRANSPOSE);
    c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = (uint8_t)(int8_t)-12;
    Transpose node(c);
    bus.note_write(0, MidiEvent{MIDI_NOTE_ON, 1, 60, 100});
    bus.note_write(0, MidiEvent{MIDI_CONTROL_CHANGE, 1, 64, 127});
    bus.note_write(0, MidiEvent{MIDI_NOTE_ON, 1, 5, 100});      // would go below 0: dropped
    bus.swap(); node.process(bus, 0); bus.swap();
    TEST_ASSERT_EQUAL(2, bus.note_count(1));
    TEST_ASSERT_EQUAL(48, bus.note_read(1, 0).data1);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, bus.note_read(1, 1).type);
    TEST_ASSERT_EQUAL(64, bus.note_read(1, 1).data1);
}

static void test_arpeggiator_plays_held_chord_ascending_on_each_edge() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.out_bus[0] = 1;   // note bus 0 held, gate bus 0 advance
    Arpeggiator node(c);
    bus.note_write(0, MidiEvent{MIDI_NOTE_ON, 2, 67, 100});
    bus.note_write(0, MidiEvent{MIDI_NOTE_ON, 2, 60, 100});
    bus.note_write(0, MidiEvent{MIDI_NOTE_ON, 2, 64, 100});
    bus.swap(); node.process(bus, 0); bus.swap();
    TEST_ASSERT_EQUAL(3, node.held_count());
    TEST_ASSERT_EQUAL(0, bus.note_count(1));

    const uint8_t expected[4] = {60, 64, 67, 60};
    uint8_t sounding = 0;
    for (uint8_t step = 0; step < 4; step++) {
        bus.gate_write(0, true); bus.swap(); node.process(bus, 0); bus.swap();   // rising edge
        const uint8_t n = bus.note_count(1);
        if (step == 0) {
            TEST_ASSERT_EQUAL(1, n);
        } else {
            TEST_ASSERT_EQUAL(2, n);
            TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, bus.note_read(1, 0).type);
            TEST_ASSERT_EQUAL(sounding, bus.note_read(1, 0).data1);
        }
        const MidiEvent& on = bus.note_read(1, n - 1);
        TEST_ASSERT_EQUAL(MIDI_NOTE_ON, on.type);
        TEST_ASSERT_EQUAL(expected[step], on.data1);
        TEST_ASSERT_EQUAL(2, on.channel);
        sounding = on.data1;
        bus.swap(); node.process(bus, 0); bus.swap();                             // gate low
        TEST_ASSERT_EQUAL(0, bus.note_count(1));
    }

    // Releasing the chord releases the sounding note.
    bus.note_write(0, MidiEvent{MIDI_NOTE_OFF, 2, 60, 0});
    bus.note_write(0, MidiEvent{MIDI_NOTE_OFF, 2, 64, 0});
    bus.note_write(0, MidiEvent{MIDI_NOTE_ON, 2, 67, 0});       // running-status note off
    bus.swap(); node.process(bus, 0); bus.swap();
    TEST_ASSERT_EQUAL(0, node.held_count());
    TEST_ASSERT_EQUAL(1, bus.note_count(1));
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, bus.note_read(1, 0).type);
    TEST_ASSERT_EQUAL(sounding, bus.note_read(1, 0).data1);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static void test_registry_descriptors_are_consistent() {
    TEST_ASSERT_TRUE(registry::count() >= 11);
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_EQUAL_PTR(d, registry::find(d->id));
        TEST_ASSERT_TRUE(d->n_in <= MAX_IN);
        TEST_ASSERT_TRUE(d->min_in <= d->n_in);
        TEST_ASSERT_TRUE(d->n_out <= MAX_OUT);
        TEST_ASSERT_TRUE(d->n_params <= N_PARAM);
        TEST_ASSERT_TRUE(d->state_size <= NODE_SLOT_SIZE);
        TEST_ASSERT_NOT_NULL(d->construct);
    }
    // Ids are part of the preset format: two algorithms sharing one would
    // silently change what a stored patch means.
    for (uint8_t i = 0; i < registry::count(); i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < registry::count(); j++) {
            TEST_ASSERT_NOT_EQUAL(registry::at(i)->id, registry::at(j)->id);
        }
    }
    TEST_ASSERT_NULL(registry::find(ALGO_NONE));
    TEST_ASSERT_NULL(registry::find(200));
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
    RUN_TEST(test_unconnected_inlets_are_skipped);
    RUN_TEST(test_sustain_debounce_yields_exactly_one_cc);
    RUN_TEST(test_sustain_params_channel_controller_invert);
    RUN_TEST(test_gate_to_note_edges);
    RUN_TEST(test_transpose_shifts_notes_and_passes_the_rest);
    RUN_TEST(test_arpeggiator_plays_held_chord_ascending_on_each_edge);
    RUN_TEST(test_registry_descriptors_are_consistent);
    return UNITY_END();
}
