// The trig conditions, and the two nodes that ask them.
//
// One suite for both, deliberately: the point of TrigCondition is that
// Probability and GateProbability answer the same question identically, and
// a test that only ever asked one of them would not notice the day they
// stopped agreeing.
//
// It also asserts the conditions that were *not* imported are reachable
// without them - a neighbour rule and a fill button are `decision`, an AND
// and a NOT - because that is the whole argument for leaving them out.

#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/note_event.h"
#include "algorithm/util/trig_condition.h"
#include "algorithm/midi/probability.h"
#include "algorithm/util/gate_probability.h"
#include "algorithm/logic/gates.h"

void setUp() {}
void tearDown() {}

// Buses, shared by every test here.
//   note 0 in, note 1 out
//   note 0 in, note 1 out, note 2 dropped
//   gate 0 in, gate 1 out, gate 2 decision, gate 3 reset, gate 4..7 spare
enum : uint8_t { B_IN = 0, B_OUT = 1, B_DECISION = 2, B_RESET = 3, B_NOTE_DROPPED = 2 };

static MidiEvent on(uint8_t note, uint8_t velocity = 100, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_ON, channel, note, velocity};
}
static MidiEvent off(uint8_t note, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_OFF, channel, note, 0};
}

// What MixedModeMaster's schedule does between two nodes: the first node's
// outlets reach the front buffer before the second node reads them, so a
// signal crosses the graph in the pass that produced it (bus/bus_manager.h).
static void publish(BusManager& bus, uint8_t gate, uint8_t note = 0xFF) {
    bus.publish(1u << gate, note == 0xFF ? 0u : (uint16_t)(1u << note), 0);
}

// One pass around a node, exactly as the master runs it.
static void pass(BusManager& bus, Node& node) {
    bus.swap();
    node.process(bus, 0);
    bus.swap();
}

// What reached one note bus this pass.
static std::vector<MidiEvent> drain(BusManager& bus, uint8_t note_bus) {
    std::vector<MidiEvent> out;
    const uint8_t n = bus.note_count(note_bus);
    for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(note_bus, i));
    return out;
}

static std::vector<MidiEvent> note_pass(BusManager& bus, Node& node) {
    pass(bus, node);
    return drain(bus, B_OUT);
}

// A trigger: high for one pass, low for the next. Returns whether the node
// let the high pass through.
static bool gate_trigger(BusManager& bus, Node& node) {
    bus.gate_write(B_IN, true);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    const bool passed = bus.gate_read(B_OUT);
    bus.swap();                    // the input falls
    node.process(bus, 0);
    bus.swap();
    return passed;
}

// The block's three parameters, by index, so a test reads as the controls do.
enum : uint16_t { P_CHANCE = 0, P_CONDITION = 1, P_SEED = 2 };

// The condition enum, by name, so a test says "3:4" rather than 11.
enum : uint8_t {
    C_1_2 = TrigCondition::COND_RATIO,      // 1:2
    C_2_2, C_1_3, C_2_3, C_3_3,
    C_1_4, C_2_4, C_3_4, C_4_4,
};

// ---------------------------------------------------------------------------
// TrigCondition: the list
// ---------------------------------------------------------------------------

// The enum is what a user reads, so its order is part of the parameter.
// Anything that is not a ratio decodes as 1:1, which is what lets the
// evaluator skip the test instead of branching on it.
static void test_the_condition_enum_is_the_list_it_names() {
    uint8_t x = 0, y = 0;
    for (uint8_t stored = TrigCondition::COND_ALWAYS; stored < TrigCondition::COND_RATIO; stored++) {
        TrigCondition::ratio_pair(stored, x, y);
        TEST_ASSERT_EQUAL(1, x);
        TEST_ASSERT_EQUAL(1, y);
    }
    // Every ratio entry is a legal pair, X within Y, in order.
    uint8_t expect_y = 2, expect_x = 1;
    for (uint8_t stored = TrigCondition::COND_RATIO; stored <= TrigCondition::COND_COUNT; stored++) {
        TrigCondition::ratio_pair(stored, x, y);
        TEST_ASSERT_EQUAL(expect_x, x);
        TEST_ASSERT_EQUAL(expect_y, y);
        TEST_ASSERT_TRUE(x >= 1 && x <= y);
        if (++expect_x > expect_y) { expect_x = 1; expect_y++; }
    }
    TEST_ASSERT_EQUAL(TrigCondition::RATIO_MAX_Y, expect_y - 1);   // it ends at 8:8
}

// The option table has to have one name per value, or the editor shows the
// wrong label for every entry after the gap.
static void test_the_block_describes_itself() {
    const ParamDescriptor& cond = TrigCondition::PARAMS[P_CONDITION];
    TEST_ASSERT_EQUAL(PARAM_ENUM, cond.kind);
    TEST_ASSERT_NOT_NULL(cond.options);
    TEST_ASSERT_EQUAL_STRING("always", cond.options[TrigCondition::COND_ALWAYS - cond.min]);
    TEST_ASSERT_EQUAL_STRING("first", cond.options[TrigCondition::COND_FIRST - cond.min]);
    TEST_ASSERT_EQUAL_STRING("not first", cond.options[TrigCondition::COND_NOT_FIRST - cond.min]);
    TEST_ASSERT_EQUAL_STRING("1:2", cond.options[TrigCondition::COND_RATIO - cond.min]);
    TEST_ASSERT_EQUAL_STRING("3:4", cond.options[C_3_4 - cond.min]);
    TEST_ASSERT_EQUAL_STRING("8:8", cond.options[TrigCondition::COND_COUNT - cond.min]);

    // An unconfigured node passes everything rather than silencing the patch.
    TEST_ASSERT_EQUAL(100, TrigCondition::PARAMS[P_CHANCE].def);
    TEST_ASSERT_EQUAL(TrigCondition::COND_ALWAYS, TrigCondition::PARAMS[P_CONDITION].def);
}

// ---------------------------------------------------------------------------
// TrigCondition: the rules
// ---------------------------------------------------------------------------

static TrigCondition make(uint8_t chance = 0, uint8_t condition = 0) {
    const uint8_t params[TrigCondition::N_PARAMS] = {chance, condition, 0};
    return TrigCondition(params);
}

// Of every Y events that reach the node, the Xth passes - which is what the
// notation means on the device it is borrowed from, where the count is of
// pattern loops and the trig comes round once per loop.
static void test_a_ratio_passes_the_xth_of_every_y() {
    TrigCondition first_of_two = make(0, C_1_2);
    TrigCondition second_of_two = make(0, C_2_2);
    TrigCondition third_of_four = make(0, C_3_4);
    for (uint8_t i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL(i % 2 == 0, first_of_two.evaluate());
        TEST_ASSERT_EQUAL(i % 2 == 1, second_of_two.evaluate());
        TEST_ASSERT_EQUAL(i % 4 == 2, third_of_four.evaluate());
    }
}

// The counter wraps on a multiple of every Y a ratio can name, so a patch
// left running does not find its ratio stepping sideways at the wrap.
static void test_the_count_wraps_without_moving_the_ratio() {
    TrigCondition c = make(0, C_3_3);
    for (uint16_t i = 0; i < TrigCondition::RATIO_CYCLE * 2 + 5; i++) {
        TEST_ASSERT_EQUAL(i % 3 == 2, c.evaluate());
    }
    for (uint8_t y = 1; y <= TrigCondition::RATIO_MAX_Y; y++) {
        TEST_ASSERT_EQUAL(0, TrigCondition::RATIO_CYCLE % y);
    }
}

// `first` fires once and never again; `not first` is its complement. Neither
// is any X:Y, because a ratio recurs and these do not.
static void test_first_and_not_first() {
    TrigCondition only_first = make(0, TrigCondition::COND_FIRST);
    TrigCondition never_first = make(0, TrigCondition::COND_NOT_FIRST);
    TEST_ASSERT_TRUE(only_first.evaluate());
    TEST_ASSERT_FALSE(never_first.evaluate());
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_FALSE(only_first.evaluate());
        TEST_ASSERT_TRUE(never_first.evaluate());
    }
}

// The two controls AND: the count says whether the moment is right, and only
// then are the dice thrown.
static void test_the_chance_and_the_condition_and_together() {
    TrigCondition c = make(100, C_3_4);
    for (uint8_t i = 0; i < 8; i++) TEST_ASSERT_EQUAL(i % 4 == 2, c.evaluate());

    TrigCondition thin = make(1, C_1_2);            // 1% of the half it allows
    uint16_t passed = 0;
    for (uint16_t i = 0; i < 400; i++) if (thin.evaluate()) passed++;
    TEST_ASSERT_TRUE(passed < 40);                  // well under the 200 the ratio allows
}

// A ratio is a position in a cycle, not a tally of what got through: the
// count advances on every event the node sees, so a chance that refuses one
// does not shift where 2:4 falls.
static void test_the_count_is_of_events_not_of_passes() {
    TrigCondition c = make(0, C_2_4);
    TEST_ASSERT_FALSE(c.evaluate());                // position 0
    TEST_ASSERT_TRUE(c.evaluate());                 // position 1
    TEST_ASSERT_EQUAL(2, c.position());

    TrigCondition never = make(1, C_2_4);           // the dice refuse nearly all
    for (uint8_t i = 0; i < 40; i++) never.evaluate();
    TEST_ASSERT_EQUAL(40 % TrigCondition::RATIO_CYCLE, never.position());
}

static void test_a_reset_puts_the_count_back_on_the_downbeat() {
    TrigCondition c = make(0, TrigCondition::COND_FIRST);
    TEST_ASSERT_TRUE(c.evaluate());
    for (uint8_t i = 0; i < 5; i++) TEST_ASSERT_FALSE(c.evaluate());
    c.reset();
    TEST_ASSERT_EQUAL(0, c.position());
    TEST_ASSERT_TRUE(c.evaluate());
}

// Writing the seed re-seeds, which is the point of a knob on it: two nodes at
// the same odds are made to disagree from the host.
static void test_the_seed_reseeds_and_the_rest_moves_freely() {
    TrigCondition c = make();
    TEST_ASSERT_TRUE(c.set_param(P_CHANCE, 50));
    TEST_ASSERT_EQUAL(50, c.get_param(P_CHANCE));
    TEST_ASSERT_TRUE(c.set_param(P_CONDITION, C_2_3));
    TEST_ASSERT_EQUAL(C_2_3, c.get_param(P_CONDITION));
    TEST_ASSERT_TRUE(c.set_param(P_SEED, 21));
    TEST_ASSERT_EQUAL(21, c.get_param(P_SEED));

    // Out of range is refused rather than clamped: the caller reports, it
    // never guesses (node/node.h).
    TEST_ASSERT_FALSE(c.set_param(P_CONDITION, (uint8_t)(TrigCondition::COND_COUNT + 1)));
    TEST_ASSERT_FALSE(c.set_param(P_CONDITION, 0));
    TEST_ASSERT_FALSE(c.set_param(TrigCondition::N_PARAMS, 1));
}

// ---------------------------------------------------------------------------
// Probability: the note side
// ---------------------------------------------------------------------------

static NodeConfig probability_config() {
    NodeConfig c = node_config(ALGO_PROBABILITY);
    c.in_buses[0] = one_bus(B_IN);
    c.out_buses[0] = one_bus(B_OUT);
    c.out_buses[2] = one_bus(B_DECISION);
    return c;
}

// The condition decides the note-on, and the note-off goes wherever its
// note-on went - the rule the node had for the dice, now for both controls.
static void test_a_ratio_thins_notes_and_keeps_their_note_offs() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.params[P_CONDITION] = C_1_2;
    Probability node(c);

    for (uint8_t i = 0; i < 8; i++) {
        const uint8_t note = (uint8_t)(60 + i);
        bus.note_write(B_IN, on(note));
        const std::vector<MidiEvent> ons = note_pass(bus, node);
        bus.note_write(B_IN, off(note));
        const std::vector<MidiEvent> offs = note_pass(bus, node);
        TEST_ASSERT_EQUAL(i % 2 == 0 ? 1 : 0, ons.size());
        TEST_ASSERT_EQUAL(ons.size(), offs.size());      // paired, either way
    }
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// The rule is about notes. A CC or a bend crossing the node is not thinned
// and does not move the count under the notes.
static void test_other_events_pass_and_do_not_advance_the_count() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.params[P_CONDITION] = C_1_2;
    Probability node(c);

    bus.note_write(B_IN, on(60));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());   // the 1st of 2

    for (uint8_t i = 0; i < 5; i++) {
        bus.note_write(B_IN, MidiEvent{MIDI_CONTROL_CHANGE, 1, 74, (uint8_t)i});
        TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());
    }
    bus.note_write(B_IN, on(62));
    TEST_ASSERT_EQUAL(0, note_pass(bus, node).size());   // still the 2nd of 2
}

// The reset inlet is the module's own idiom - every sequencer has one, and
// Transport's start outlet patched there is how a patch plays from the top
// when the DAW does. A reset in the same pass as a note-on is applied first.
static void test_the_reset_inlet_returns_the_count_to_the_top() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.in_buses[1] = one_bus(B_RESET);
    c.params[P_CONDITION] = TrigCondition::COND_FIRST;
    Probability node(c);

    bus.note_write(B_IN, on(60));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());
    bus.note_write(B_IN, on(61));
    TEST_ASSERT_EQUAL(0, note_pass(bus, node).size());

    bus.gate_write(B_RESET, true);                       // a rising edge, with a note
    bus.note_write(B_IN, on(62));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());

    bus.gate_write(B_RESET, true);                       // still high: not an edge
    bus.note_write(B_IN, on(63));
    TEST_ASSERT_EQUAL(0, note_pass(bus, node).size());
}


// The other half of the rule: a refused note-on leaves by `dropped` rather
// than nowhere, unchanged, and its note-off follows it there. Nothing is
// lost - the line is split, not holed.
static void test_refused_notes_leave_by_the_dropped_outlet() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.out_buses[1] = one_bus(B_NOTE_DROPPED);
    c.params[P_CONDITION] = C_1_2;
    Probability node(c);

    for (uint8_t i = 0; i < 8; i++) {
        const uint8_t note = (uint8_t)(60 + i);
        const bool kept = (i % 2) == 0;

        bus.note_write(B_IN, on(note, 99, 3));
        pass(bus, node);
        const std::vector<MidiEvent> kept_on = drain(bus, B_OUT);
        const std::vector<MidiEvent> lost_on = drain(bus, B_NOTE_DROPPED);
        TEST_ASSERT_EQUAL(kept ? 1 : 0, kept_on.size());
        TEST_ASSERT_EQUAL(kept ? 0 : 1, lost_on.size());
        if (!kept) {
            // The node routes; it does not modify. Pitch, velocity and
            // channel arrive as they were sent.
            TEST_ASSERT_TRUE(is_note_on(lost_on[0]));
            TEST_ASSERT_EQUAL(note, lost_on[0].data1);
            TEST_ASSERT_EQUAL(99, lost_on[0].data2);
            TEST_ASSERT_EQUAL(3, lost_on[0].channel);
        }

        bus.note_write(B_IN, off(note, 3));
        pass(bus, node);
        const std::vector<MidiEvent> kept_off = drain(bus, B_OUT);
        const std::vector<MidiEvent> lost_off = drain(bus, B_NOTE_DROPPED);
        TEST_ASSERT_EQUAL(kept ? 1 : 0, kept_off.size());
        TEST_ASSERT_EQUAL(kept ? 0 : 1, lost_off.size());
        if (!kept) {
            TEST_ASSERT_TRUE(is_note_off(lost_off[0]));
            TEST_ASSERT_EQUAL(note, lost_off[0].data1);
            TEST_ASSERT_EQUAL(3, lost_off[0].channel);
        }
    }
    TEST_ASSERT_EQUAL(0, node.sounding_count());         // neither side hangs
    TEST_ASSERT_EQUAL(0, node.dropped_count());
}

// A CC was never refused, so it is not on the other half of a rule that was
// never asked about it: it leaves by `notes out` alone.
static void test_other_events_are_not_copied_to_dropped() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.out_buses[1] = one_bus(B_NOTE_DROPPED);
    c.params[P_CHANCE] = 1;
    c.params[P_CONDITION] = C_2_2;                       // the 2nd of every 2
    Probability node(c);

    for (uint8_t i = 0; i < 4; i++) {
        bus.note_write(B_IN, MidiEvent{MIDI_CONTROL_CHANGE, 1, 74, (uint8_t)i});
        pass(bus, node);
        TEST_ASSERT_EQUAL(1, drain(bus, B_OUT).size());
        TEST_ASSERT_EQUAL(0, drain(bus, B_NOTE_DROPPED).size());
    }
}

// Nothing patched there is nothing recorded: a ledger filling up with notes
// no bus carries would start refusing the ones that are heard.
static void test_an_unpatched_dropped_outlet_records_nothing() {
    BusManager bus;
    NodeConfig c = probability_config();                 // out_buses[1] is unconnected
    c.params[P_CONDITION] = TrigCondition::COND_FIRST;   // one passes, the rest do not
    Probability node(c);

    for (uint8_t i = 0; i < SoundingNotes::CAPACITY * 2; i++) {
        const uint8_t note = (uint8_t)(40 + i % 40);
        bus.note_write(B_IN, on(note));
        const size_t played = note_pass(bus, node).size();
        TEST_ASSERT_EQUAL(i == 0 ? 1 : 0, played);
        bus.note_write(B_IN, off(note));
        TEST_ASSERT_EQUAL(played, note_pass(bus, node).size());
        TEST_ASSERT_EQUAL(0, node.dropped_count());      // never recorded, never refused
    }
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// A handover releases both ledgers, because a note held on the dropped side
// hangs a synth exactly as one held on the other (node/node.h).
static void test_a_handover_releases_both_sides() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.out_buses[1] = one_bus(B_NOTE_DROPPED);
    c.params[P_CONDITION] = C_1_2;
    Probability node(c);

    bus.note_write(B_IN, on(60));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());   // the 1st of 2: kept
    bus.note_write(B_IN, on(61));
    pass(bus, node);
    TEST_ASSERT_EQUAL(1, drain(bus, B_NOTE_DROPPED).size());   // the 2nd: dropped
    TEST_ASSERT_EQUAL(1, node.sounding_count());
    TEST_ASSERT_EQUAL(1, node.dropped_count());

    bus.swap();
    node.silence(bus);
    bus.swap();
    const std::vector<MidiEvent> kept = drain(bus, B_OUT);
    const std::vector<MidiEvent> lost = drain(bus, B_NOTE_DROPPED);
    TEST_ASSERT_EQUAL(1, kept.size());
    TEST_ASSERT_TRUE(is_note_off(kept[0]));
    TEST_ASSERT_EQUAL(60, kept[0].data1);
    TEST_ASSERT_EQUAL(1, lost.size());
    TEST_ASSERT_TRUE(is_note_off(lost[0]));
    TEST_ASSERT_EQUAL(61, lost[0].data1);
}

// ---------------------------------------------------------------------------
// GateProbability: the gate side
// ---------------------------------------------------------------------------

static NodeConfig gate_probability_config() {
    NodeConfig c = node_config(ALGO_GATE_PROBABILITY);
    c.in_buses[0] = one_bus(B_IN);
    c.out_buses[0] = one_bus(B_OUT);
    c.out_buses[2] = one_bus(B_DECISION);
    return c;
}

// The same rule, counted in rising edges rather than note-ons: a hat and a
// melody set to the same ratio fall on the same beats.
static void test_gate_ratio_counts_rising_edges() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.params[P_CONDITION] = C_3_4;
    GateProbability node(c);
    for (uint8_t i = 0; i < 12; i++) TEST_ASSERT_EQUAL(i % 4 == 2, gate_trigger(bus, node));
}

// The decision is taken on the edge and held for the whole of the gate, so a
// passed gate is exactly as long as its input and a dropped one never
// flickers - the level equivalent of pairing a note-off with its note-on.
static void test_the_decision_is_held_for_the_length_of_the_gate() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.params[P_CONDITION] = C_1_2;                       // pass, drop, pass...
    GateProbability node(c);

    for (uint8_t edge = 0; edge < 4; edge++) {
        const bool expect = (edge % 2) == 0;
        for (uint8_t held = 0; held < 5; held++) {       // one long gate
            bus.gate_write(B_IN, true);
            bus.swap(); node.process(bus, 0); bus.swap();
            TEST_ASSERT_EQUAL(expect, bus.gate_read(B_OUT));
            TEST_ASSERT_EQUAL(expect, node.open());
        }
        bus.swap(); node.process(bus, 0); bus.swap();    // the input falls
        TEST_ASSERT_FALSE(bus.gate_read(B_OUT));
        TEST_ASSERT_FALSE(node.open());
    }
}

// A gate held up for ever is one event, not one per pass: the count only
// moves on an edge.
static void test_a_held_gate_is_one_event() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.params[P_CONDITION] = C_2_2;
    GateProbability node(c);

    for (uint8_t i = 0; i < 10; i++) {                   // held, never falls
        bus.gate_write(B_IN, true);
        bus.swap(); node.process(bus, 0); bus.swap();
        TEST_ASSERT_FALSE(bus.gate_read(B_OUT));         // still the 1st of 2
    }
    bus.swap(); node.process(bus, 0); bus.swap();        // falls
    TEST_ASSERT_TRUE(gate_trigger(bus, node));           // the 2nd of 2
}

static void test_a_gate_node_defaults_to_passing_everything() {
    BusManager bus;
    GateProbability node(gate_probability_config());     // params zeroed
    for (uint8_t i = 0; i < 10; i++) TEST_ASSERT_TRUE(gate_trigger(bus, node));
}

static void test_the_gate_reset_inlet_returns_the_count_to_the_top() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.in_buses[1] = one_bus(B_RESET);
    c.params[P_CONDITION] = TrigCondition::COND_FIRST;
    GateProbability node(c);

    TEST_ASSERT_TRUE(gate_trigger(bus, node));
    TEST_ASSERT_FALSE(gate_trigger(bus, node));

    bus.gate_write(B_RESET, true);
    bus.swap(); node.process(bus, 0); bus.swap();        // the reset edge alone
    bus.swap(); node.process(bus, 0); bus.swap();        // and it falls
    TEST_ASSERT_TRUE(gate_trigger(bus, node));
}


// The same split, said in levels: `gate out` and `dropped` are one pulse
// train cut in two. Exactly one of them is up while the input is, neither is
// up between two gates, and both hold for the whole of the gate they were
// decided on.
static void test_refused_gates_leave_by_the_dropped_outlet() {
    BusManager bus;
    enum : uint8_t { B_DROPPED = 4 };
    NodeConfig c = gate_probability_config();
    c.out_buses[1] = one_bus(B_DROPPED);
    c.params[P_CONDITION] = C_1_2;                       // pass, drop, pass...
    GateProbability node(c);

    for (uint8_t edge = 0; edge < 4; edge++) {
        const bool kept = (edge % 2) == 0;
        for (uint8_t held = 0; held < 5; held++) {       // one long gate
            bus.gate_write(B_IN, true);
            bus.swap(); node.process(bus, 0); bus.swap();
            TEST_ASSERT_EQUAL(kept, bus.gate_read(B_OUT));
            TEST_ASSERT_EQUAL(!kept, bus.gate_read(B_DROPPED));
            TEST_ASSERT_EQUAL(!kept, node.blocked());
        }
        bus.swap(); node.process(bus, 0); bus.swap();    // the input falls
        TEST_ASSERT_FALSE(bus.gate_read(B_OUT));
        TEST_ASSERT_FALSE(bus.gate_read(B_DROPPED));     // never between two gates
        TEST_ASSERT_FALSE(node.blocked());
    }
}

// What the outlet is worth, stated as the patch it replaces: `dropped` is
// `AND(this node's input, NOT decision)`, pass for pass. The note side has
// no such patch - nothing downstream can tell a refused note-on from one
// that was never played - which is why the pair carries the outlet on both.
static void test_a_dropped_gate_is_the_patch_it_saves() {
    BusManager bus;
    enum : uint8_t { B_DROPPED = 4, B_NOT = 5, B_ANDED = 6 };

    NodeConfig c = gate_probability_config();
    c.out_buses[1] = one_bus(B_DROPPED);
    c.params[P_CHANCE] = 50;
    c.params[P_SEED] = 7;
    GateProbability node(c);

    NodeConfig invert = node_config(ALGO_LOGIC_NOT);
    invert.in_buses[0] = one_bus(B_DECISION); invert.out_buses[0] = one_bus(B_NOT);
    LogicNot inverter(invert);

    NodeConfig conjoin = node_config(ALGO_LOGIC_AND);
    conjoin.in_buses[0] = one_bus(B_IN); conjoin.in_buses[1] = one_bus(B_NOT); conjoin.out_buses[0] = one_bus(B_ANDED);
    LogicAND conjunction(conjoin);

    uint16_t dropped = 0;
    for (uint16_t i = 0; i < 100; i++) {
        bus.gate_write(B_IN, true);
        bus.swap();
        node.process(bus, 0);
        publish(bus, B_OUT); publish(bus, B_DECISION); publish(bus, B_DROPPED);
        inverter.process(bus, 0);    publish(bus, B_NOT);
        conjunction.process(bus, 0); publish(bus, B_ANDED);
        bus.swap();
        TEST_ASSERT_EQUAL(bus.gate_read(B_ANDED), bus.gate_read(B_DROPPED));
        TEST_ASSERT_TRUE(bus.gate_read(B_OUT) != bus.gate_read(B_DROPPED));
        if (bus.gate_read(B_DROPPED)) dropped++;

        bus.swap();                                      // everything falls
        node.process(bus, 0);
        publish(bus, B_OUT); publish(bus, B_DECISION); publish(bus, B_DROPPED);
        inverter.process(bus, 0);    publish(bus, B_NOT);
        conjunction.process(bus, 0); publish(bus, B_ANDED);
        bus.swap();
    }
    TEST_ASSERT_TRUE(dropped > 20 && dropped < 80);      // the coin was actually tossed
}

// ---------------------------------------------------------------------------
// What was left out, and why
// ---------------------------------------------------------------------------

// A groovebox spends a condition on "play where the neighbouring track
// played" because its tracks cannot be wired to each other. Here `decision`
// is an outlet: AND it with a second node's input and that is the rule, with
// a NOT in front for its complement, at any distance and across both
// domains.
// This is the test that says the omission is a saving rather than a loss.
static void test_a_neighbour_rule_is_the_decision_an_and_and_a_not() {
    BusManager bus;
    enum : uint8_t { B_NOT = 4, B_ANDED = 5, B_FOLLOW_OUT = 6 };

    NodeConfig lead = gate_probability_config();
    lead.params[P_CONDITION] = C_1_2;
    GateProbability first(lead);

    NodeConfig invert = node_config(ALGO_LOGIC_NOT);
    invert.in_buses[0] = one_bus(B_DECISION); invert.out_buses[0] = one_bus(B_NOT);
    LogicNot inverter(invert);

    NodeConfig conjoin = node_config(ALGO_LOGIC_AND);
    conjoin.in_buses[0] = one_bus(B_IN); conjoin.in_buses[1] = one_bus(B_NOT); conjoin.out_buses[0] = one_bus(B_ANDED);
    LogicAND conjunction(conjoin);

    NodeConfig follow = gate_probability_config();
    follow.in_buses[0] = one_bus(B_ANDED);
    follow.out_buses[0] = one_bus(B_FOLLOW_OUT);
    follow.out_buses[2] = BusSet{};                          // a latch it does not need
    GateProbability second(follow);

    for (uint8_t i = 0; i < 8; i++) {
        bus.gate_write(B_IN, true);
        bus.swap();
        first.process(bus, 0);       publish(bus, B_DECISION, B_OUT); publish(bus, B_OUT);
        inverter.process(bus, 0);    publish(bus, B_NOT);
        conjunction.process(bus, 0); publish(bus, B_ANDED);
        second.process(bus, 0);
        bus.swap();
        // Exactly one of them plays each time, which is "not nei" by cable.
        TEST_ASSERT_EQUAL(i % 2 == 0, bus.gate_read(B_OUT));
        TEST_ASSERT_TRUE(bus.gate_read(B_OUT) != bus.gate_read(B_FOLLOW_OUT));

        bus.swap();                                      // everything falls
        first.process(bus, 0);       publish(bus, B_DECISION, B_OUT); publish(bus, B_OUT);
        inverter.process(bus, 0);    publish(bus, B_NOT);
        conjunction.process(bus, 0); publish(bus, B_ANDED);
        second.process(bus, 0);
        bus.swap();
    }
}

// The same argument for the fill button: a gate ANDed with the input, and
// the gate is whatever the patch says it is.
static void test_a_fill_button_is_a_gate_and_an_and() {
    BusManager bus;
    enum : uint8_t { B_FILL = 4, B_ANDED = 5 };

    NodeConfig conjoin = node_config(ALGO_LOGIC_AND);
    conjoin.in_buses[0] = one_bus(B_IN); conjoin.in_buses[1] = one_bus(B_FILL); conjoin.out_buses[0] = one_bus(B_ANDED);
    LogicAND conjunction(conjoin);

    NodeConfig c = gate_probability_config();
    c.in_buses[0] = one_bus(B_ANDED);
    GateProbability node(c);

    for (uint8_t i = 0; i < 8; i++) {
        const bool fill = i >= 4;                        // the button goes down halfway
        bus.gate_write(B_IN, true);
        if (fill) bus.gate_write(B_FILL, true);
        bus.swap();
        conjunction.process(bus, 0); publish(bus, B_ANDED);
        node.process(bus, 0);
        bus.swap();
        TEST_ASSERT_EQUAL(fill, bus.gate_read(B_OUT));
        bus.swap();
        conjunction.process(bus, 0); publish(bus, B_ANDED);
        node.process(bus, 0);
        bus.swap();
    }
}

// ---------------------------------------------------------------------------
// Both nodes, one block
// ---------------------------------------------------------------------------

// The point of the shared object: the same three parameters, in the same
// order, with the same descriptors, so a preset's bytes and an editor's
// controls mean the same thing on either node. What the note node adds after
// the block is its own - a gate has no channel to send on.
static void test_both_nodes_expose_one_block() {
    const AlgorithmDescriptor* notes = registry::find(ALGO_PROBABILITY);
    const AlgorithmDescriptor* gates = registry::find(ALGO_GATE_PROBABILITY);
    TEST_ASSERT_NOT_NULL(notes);
    TEST_ASSERT_NOT_NULL(gates);
    TEST_ASSERT_EQUAL(Probability::N_PARAMS, notes->n_params);
    TEST_ASSERT_EQUAL(TrigCondition::N_PARAMS, gates->n_params);
    for (uint16_t i = 0; i < TrigCondition::N_PARAMS; i++) {
        const ParamDescriptor* a = registry::param(*notes, i);
        const ParamDescriptor* b = registry::param(*gates, i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_PTR(a, b);                     // literally one table
    }
    // The block is the whole of the gate node and all but the last parameter
    // of the note node, so the shared bytes keep their meaning on both.
    TEST_ASSERT_EQUAL(TrigCondition::N_PARAMS, Probability::P_CHANNEL);
    const ParamDescriptor* channel = registry::param(*notes, Probability::P_CHANNEL);
    TEST_ASSERT_NOT_NULL(channel);
    TEST_ASSERT_EQUAL_STRING("channel", channel->name);
    TEST_ASSERT_NULL(registry::param(*gates, TrigCondition::N_PARAMS));
    // The reset inlet and the latch outlet are on both, by the same name,
    // and only the signal inlet is required.
    TEST_ASSERT_EQUAL_STRING("reset", notes->in_name[1]);
    TEST_ASSERT_EQUAL_STRING("decision", notes->out_name[2]);
    TEST_ASSERT_EQUAL_STRING("reset", gates->in_name[1]);
    TEST_ASSERT_EQUAL_STRING("decision", gates->out_name[2]);
    TEST_ASSERT_EQUAL(1, notes->min_in);
    TEST_ASSERT_EQUAL(1, gates->min_in);
    TEST_ASSERT_EQUAL(2, notes->n_in);
    TEST_ASSERT_EQUAL(2, gates->n_in);

    // Three outlets on both, in the same order: the two halves of the signal
    // first, each in the domain its node works in, and the decision last.
    TEST_ASSERT_EQUAL(3, notes->n_out);
    TEST_ASSERT_EQUAL(3, gates->n_out);
    TEST_ASSERT_EQUAL_STRING("dropped", notes->out_name[1]);
    TEST_ASSERT_EQUAL_STRING("dropped", gates->out_name[1]);
    TEST_ASSERT_TRUE(notes->out_domain[0] == Domain::Note);
    TEST_ASSERT_TRUE(notes->out_domain[1] == Domain::Note);
    TEST_ASSERT_TRUE(notes->out_domain[2] == Domain::Gate);
    TEST_ASSERT_TRUE(gates->out_domain[1] == Domain::Gate);
}

// Two nodes at the same odds and different seeds must not agree, or a patch
// that wanted two independent coins got one.
static void test_two_nodes_at_the_same_odds_can_be_made_to_disagree() {
    BusManager bus;
    NodeConfig a = gate_probability_config();
    a.params[P_CHANCE] = 50;
    a.params[P_SEED] = 1;
    NodeConfig b = a;
    b.params[P_SEED] = 200;
    b.out_buses[0] = one_bus(5);
    b.out_buses[2] = one_bus(6);
    GateProbability left(a);
    GateProbability right(b);

    uint16_t differed = 0;
    for (uint16_t i = 0; i < 200; i++) {
        bus.gate_write(B_IN, true);
        bus.swap(); left.process(bus, 0); right.process(bus, 0); bus.swap();
        if (bus.gate_read(B_OUT) != bus.gate_read(5)) differed++;
        bus.swap(); left.process(bus, 0); right.process(bus, 0); bus.swap();
    }
    TEST_ASSERT_TRUE(differed > 40);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_condition_enum_is_the_list_it_names);
    RUN_TEST(test_the_block_describes_itself);
    RUN_TEST(test_a_ratio_passes_the_xth_of_every_y);
    RUN_TEST(test_the_count_wraps_without_moving_the_ratio);
    RUN_TEST(test_first_and_not_first);
    RUN_TEST(test_the_chance_and_the_condition_and_together);
    RUN_TEST(test_the_count_is_of_events_not_of_passes);
    RUN_TEST(test_a_reset_puts_the_count_back_on_the_downbeat);
    RUN_TEST(test_the_seed_reseeds_and_the_rest_moves_freely);

    RUN_TEST(test_a_ratio_thins_notes_and_keeps_their_note_offs);
    RUN_TEST(test_other_events_pass_and_do_not_advance_the_count);
    RUN_TEST(test_the_reset_inlet_returns_the_count_to_the_top);
    RUN_TEST(test_refused_notes_leave_by_the_dropped_outlet);
    RUN_TEST(test_other_events_are_not_copied_to_dropped);
    RUN_TEST(test_an_unpatched_dropped_outlet_records_nothing);
    RUN_TEST(test_a_handover_releases_both_sides);

    RUN_TEST(test_gate_ratio_counts_rising_edges);
    RUN_TEST(test_the_decision_is_held_for_the_length_of_the_gate);
    RUN_TEST(test_a_held_gate_is_one_event);
    RUN_TEST(test_a_gate_node_defaults_to_passing_everything);
    RUN_TEST(test_the_gate_reset_inlet_returns_the_count_to_the_top);
    RUN_TEST(test_refused_gates_leave_by_the_dropped_outlet);
    RUN_TEST(test_a_dropped_gate_is_the_patch_it_saves);

    RUN_TEST(test_a_neighbour_rule_is_the_decision_an_and_and_a_not);
    RUN_TEST(test_a_fill_button_is_a_gate_and_an_and);

    RUN_TEST(test_both_nodes_expose_one_block);
    RUN_TEST(test_two_nodes_at_the_same_odds_can_be_made_to_disagree);
    return UNITY_END();
}
