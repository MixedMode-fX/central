// The trig conditions, and the two nodes that ask them.
//
// One suite for both, deliberately: the point of TrigCondition is that
// Probability and GateProbability answer the same question identically, and
// a test that only ever asked one of them would not notice the day they
// stopped agreeing.

#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/note_event.h"
#include "clock/transport_edge.h"
#include "algorithm/util/trig_condition.h"
#include "algorithm/midi/probability.h"
#include "algorithm/util/gate_probability.h"

void setUp() {}
void tearDown() {}

// Buses, shared by every test here.
//   note 0 in, note 1 out
//   gate 0 gate in, gate 1 gate out, gate 2 passed, gate 3 fill, gate 4 nei
enum : uint8_t { B_IN = 0, B_OUT = 1, B_PASSED = 2, B_FILL = 3, B_NEI = 4 };

static MidiEvent on(uint8_t note, uint8_t velocity = 100, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_ON, channel, note, velocity};
}
static MidiEvent off(uint8_t note, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_OFF, channel, note, 0};
}

// One pass around a node, exactly as MixedModeMaster runs it: the writes made
// before the call are swapped in, the node runs, and its outlets are swapped
// out and read.
// What MixedModeMaster's schedule does between two nodes: the first node's
// outlets reach the front buffer before the second node reads them, so a
// signal crosses the graph in the pass that produced it.
static void publish(BusManager& bus, uint8_t gate, uint8_t note = 0xFF) {
    bus.publish(1u << gate, note == 0xFF ? 0u : (uint16_t)(1u << note), 0);
}

static std::vector<MidiEvent> note_pass(BusManager& bus, Node& node) {
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    std::vector<MidiEvent> out;
    const uint8_t n = bus.note_count(B_OUT);
    for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(B_OUT, i));
    return out;
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

// The condition block's four parameters, by index, so a test reads as the
// controls do.
enum : uint16_t { P_CHANCE = 0, P_RATIO = 1, P_CONDITION = 2, P_SEED = 3 };

// ---------------------------------------------------------------------------
// TrigCondition: the ratio list
// ---------------------------------------------------------------------------

// The enum is what a user reads, so its order is part of the parameter. Off
// decodes as 1:1 rather than as a special case, which is what lets the
// evaluator skip the test instead of branching on it.
static void test_the_ratio_enum_is_the_list_it_names() {
    uint8_t x = 0, y = 0;
    TrigCondition::ratio_pair(TrigCondition::RATIO_OFF, x, y);
    TEST_ASSERT_EQUAL(1, x); TEST_ASSERT_EQUAL(1, y);

    TrigCondition::ratio_pair(2, x, y);   // "1:2"
    TEST_ASSERT_EQUAL(1, x); TEST_ASSERT_EQUAL(2, y);
    TrigCondition::ratio_pair(3, x, y);   // "2:2"
    TEST_ASSERT_EQUAL(2, x); TEST_ASSERT_EQUAL(2, y);
    TrigCondition::ratio_pair(4, x, y);   // "1:3"
    TEST_ASSERT_EQUAL(1, x); TEST_ASSERT_EQUAL(3, y);
    TrigCondition::ratio_pair(6, x, y);   // "3:3"
    TEST_ASSERT_EQUAL(3, x); TEST_ASSERT_EQUAL(3, y);
    TrigCondition::ratio_pair(TrigCondition::RATIO_COUNT, x, y);   // "8:8"
    TEST_ASSERT_EQUAL(8, x); TEST_ASSERT_EQUAL(8, y);

    // Every entry between them is a legal pair, X within Y, in order.
    uint8_t expect_y = 2, expect_x = 1;
    for (uint8_t stored = 2; stored <= TrigCondition::RATIO_COUNT; stored++) {
        TrigCondition::ratio_pair(stored, x, y);
        TEST_ASSERT_EQUAL(expect_x, x);
        TEST_ASSERT_EQUAL(expect_y, y);
        TEST_ASSERT_TRUE(x >= 1 && x <= y);
        if (++expect_x > expect_y) { expect_x = 1; expect_y++; }
    }
}

// The option table has to have one name per value or the editor shows the
// wrong label for every entry after the gap.
static void test_the_condition_block_describes_itself() {
    const ParamDescriptor& ratio = TrigCondition::PARAMS[P_RATIO];
    TEST_ASSERT_EQUAL(PARAM_ENUM, ratio.kind);
    TEST_ASSERT_NOT_NULL(ratio.options);
    TEST_ASSERT_EQUAL_STRING("off", ratio.options[0]);
    TEST_ASSERT_EQUAL_STRING("1:2", ratio.options[1]);
    TEST_ASSERT_EQUAL_STRING("8:8", ratio.options[TrigCondition::RATIO_COUNT - ratio.min]);

    const ParamDescriptor& cond = TrigCondition::PARAMS[P_CONDITION];
    TEST_ASSERT_EQUAL(PARAM_ENUM, cond.kind);
    TEST_ASSERT_EQUAL_STRING("always", cond.options[0]);
    TEST_ASSERT_EQUAL_STRING("not fill", cond.options[TrigCondition::COND_COUNT - cond.min]);

    // An unconfigured node passes everything rather than silencing the patch.
    TEST_ASSERT_EQUAL(100, TrigCondition::PARAMS[P_CHANCE].def);
}

// ---------------------------------------------------------------------------
// TrigCondition: the rules
// ---------------------------------------------------------------------------

static TrigCondition make(uint8_t chance = 0, uint8_t ratio = 0, uint8_t condition = 0) {
    const uint8_t params[TrigCondition::N_PARAMS] = {chance, ratio, condition, 0};
    return TrigCondition(params);
}

// Of every Y events that reach the node, the Xth passes - which is what a
// trig condition means on the device this borrows the notation from, where
// the count is of pattern loops and the trig comes round once per loop.
static void test_a_ratio_passes_the_xth_of_every_y() {
    TrigCondition first_of_two = make(0, 2);             // 1:2
    TrigCondition second_of_two = make(0, 3);            // 2:2
    TrigCondition third_of_four = make(0, 9);            // 3:4
    for (uint8_t i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL(i % 2 == 0, first_of_two.evaluate(false, false));
        TEST_ASSERT_EQUAL(i % 2 == 1, second_of_two.evaluate(false, false));
        TEST_ASSERT_EQUAL(i % 4 == 2, third_of_four.evaluate(false, false));
    }
}

// The counter wraps on a multiple of every Y a ratio can name, so a patch
// left running does not find its ratio stepping sideways at the wrap.
static void test_the_count_wraps_without_moving_the_ratio() {
    TrigCondition c = make(0, 6);                        // 3:3
    for (uint16_t i = 0; i < TrigCondition::RATIO_CYCLE * 2 + 5; i++) {
        TEST_ASSERT_EQUAL(i % 3 == 2, c.evaluate(false, false));
    }
    TEST_ASSERT_EQUAL(TrigCondition::RATIO_MAX_Y, 8);
    for (uint8_t y = 1; y <= TrigCondition::RATIO_MAX_Y; y++) {
        TEST_ASSERT_EQUAL(0, TrigCondition::RATIO_CYCLE % y);
    }
}

static void test_first_and_not_first() {
    TrigCondition only_first = make(0, 0, TrigCondition::COND_FIRST);
    TrigCondition never_first = make(0, 0, TrigCondition::COND_NOT_FIRST);
    TEST_ASSERT_TRUE(only_first.evaluate(false, false));
    TEST_ASSERT_FALSE(never_first.evaluate(false, false));
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_FALSE(only_first.evaluate(false, false));
        TEST_ASSERT_TRUE(never_first.evaluate(false, false));
    }
}

// `pre` is the node's own last decision, which is what makes a pair of nodes
// answer each other: at 50% one plays where the other did not.
static void test_pre_follows_the_last_decision() {
    TrigCondition c = make(0, 0, TrigCondition::COND_PRE);
    // last_pass starts false, so `pre` refuses until something sets it.
    TEST_ASSERT_FALSE(c.evaluate(false, false));
    TEST_ASSERT_FALSE(c.evaluate(false, false));

    TrigCondition not_pre = make(0, 0, TrigCondition::COND_NOT_PRE);
    TEST_ASSERT_TRUE(not_pre.evaluate(false, false));    // nothing passed yet
    TEST_ASSERT_FALSE(not_pre.evaluate(false, false));   // ...but now it has
    TEST_ASSERT_TRUE(not_pre.evaluate(false, false));
    TEST_ASSERT_FALSE(not_pre.evaluate(false, false));
}

static void test_fill_and_nei_come_from_the_inlets() {
    TrigCondition fill = make(0, 0, TrigCondition::COND_FILL);
    TrigCondition not_fill = make(0, 0, TrigCondition::COND_NOT_FILL);
    TrigCondition nei = make(0, 0, TrigCondition::COND_NEI);
    TrigCondition not_nei = make(0, 0, TrigCondition::COND_NOT_NEI);

    TEST_ASSERT_TRUE(fill.evaluate(true, false));
    TEST_ASSERT_FALSE(fill.evaluate(false, true));
    TEST_ASSERT_TRUE(not_fill.evaluate(false, true));
    TEST_ASSERT_FALSE(not_fill.evaluate(true, false));
    TEST_ASSERT_TRUE(nei.evaluate(false, true));
    TEST_ASSERT_FALSE(nei.evaluate(true, false));
    TEST_ASSERT_TRUE(not_nei.evaluate(true, false));
    TEST_ASSERT_FALSE(not_nei.evaluate(false, true));
}

// The three controls compose: a ratio says where in the cycle, a condition
// says whether the moment is right, and only then are the dice thrown. That
// is the difference from the device this borrows from, where a trig has one
// condition and "half the time, on the first of every four" is not sayable.
static void test_the_three_controls_and_together() {
    TrigCondition c = make(100, 9, TrigCondition::COND_NOT_FILL);   // 3:4
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(i % 4 == 2, c.evaluate(false, false));
    }
    for (uint8_t i = 0; i < 8; i++) {                               // fill high
        TEST_ASSERT_FALSE(c.evaluate(true, false));
    }

    TrigCondition never = make(1, 2);                               // 1% at 1:2
    uint16_t passed = 0;
    for (uint16_t i = 0; i < 400; i++) if (never.evaluate(false, false)) passed++;
    TEST_ASSERT_TRUE(passed < 40);                                  // well under the 200 the ratio allows
}

// A ratio is a position in a cycle, not a tally of what got through: the
// count advances on every event the node sees, so turning a condition on and
// off does not shift where 2:4 falls.
static void test_the_count_is_of_events_not_of_passes() {
    TrigCondition c = make(0, 8);                        // 2:4
    c.evaluate(false, false);                            // position 0, refused
    TEST_ASSERT_TRUE(c.evaluate(false, false));          // position 1, passes
    TEST_ASSERT_EQUAL(2, c.position());

    TrigCondition fill = make(0, 8, TrigCondition::COND_FILL);
    for (uint8_t i = 0; i < 4; i++) fill.evaluate(false, false);    // all refused
    TEST_ASSERT_EQUAL(4, fill.position());
    TEST_ASSERT_FALSE(fill.evaluate(true, false));       // position 4 -> not the 2nd
    TEST_ASSERT_TRUE(fill.evaluate(true, false));        // position 5 -> it is
}

static void test_a_restart_puts_the_count_back_on_the_downbeat() {
    TrigCondition c = make(0, 2, TrigCondition::COND_FIRST);   // 1:2 and first
    TEST_ASSERT_TRUE(c.evaluate(false, false));
    for (uint8_t i = 0; i < 5; i++) TEST_ASSERT_FALSE(c.evaluate(false, false));
    c.restart();
    TEST_ASSERT_EQUAL(0, c.position());
    TEST_ASSERT_TRUE(c.evaluate(false, false));
}

// Writing the seed re-seeds, which is the point of a knob on it: two nodes at
// the same odds are made to disagree from the host.
static void test_the_seed_reseeds_and_the_rest_moves_freely() {
    TrigCondition c = make();
    TEST_ASSERT_TRUE(c.set_param(P_CHANCE, 50));
    TEST_ASSERT_EQUAL(50, c.get_param(P_CHANCE));
    TEST_ASSERT_TRUE(c.set_param(P_RATIO, 5));
    TEST_ASSERT_EQUAL(5, c.get_param(P_RATIO));
    TEST_ASSERT_TRUE(c.set_param(P_CONDITION, TrigCondition::COND_NOT_PRE));
    TEST_ASSERT_EQUAL(TrigCondition::COND_NOT_PRE, c.get_param(P_CONDITION));
    TEST_ASSERT_TRUE(c.set_param(P_SEED, 21));
    TEST_ASSERT_EQUAL(21, c.get_param(P_SEED));

    // Out of range is refused rather than clamped: the caller reports, it
    // never guesses (node/node.h).
    TEST_ASSERT_FALSE(c.set_param(P_RATIO, (uint8_t)(TrigCondition::RATIO_COUNT + 1)));
    TEST_ASSERT_FALSE(c.set_param(P_CONDITION, (uint8_t)(TrigCondition::COND_COUNT + 1)));
    TEST_ASSERT_FALSE(c.set_param(TrigCondition::N_PARAMS, 1));
}

// ---------------------------------------------------------------------------
// Probability: the note side
// ---------------------------------------------------------------------------

static NodeConfig probability_config() {
    NodeConfig c = node_config(ALGO_PROBABILITY);
    c.in_bus[0] = B_IN;
    c.out_bus[0] = B_OUT;
    c.out_bus[1] = B_PASSED;
    return c;
}

// The ratio decides the note-on, and the note-off goes wherever its note-on
// went - which is the rule the node had for the dice and now has for all
// three controls.
static void test_a_ratio_thins_notes_and_keeps_their_note_offs() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.params[P_RATIO] = 2;                               // 1:2
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

// The fill inlet is the device's fill button, as a cable: anything that
// writes a gate bus can be it.
static void test_the_fill_inlet_arms_the_condition() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.in_bus[1] = B_FILL;
    c.params[P_CONDITION] = TrigCondition::COND_FILL;
    Probability node(c);

    bus.note_write(B_IN, on(60));
    TEST_ASSERT_EQUAL(0, note_pass(bus, node).size());   // fill low

    bus.gate_write(B_FILL, true);
    bus.note_write(B_IN, on(61));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());
}

// `passed` is latched, not a pulse: a neighbour clocked in some other pass
// still reads the last decision.
static void test_passed_is_latched_and_drives_a_neighbour() {
    BusManager bus;
    NodeConfig lead = probability_config();
    lead.params[P_RATIO] = 2;                            // 1:2
    Probability first(lead);

    NodeConfig follow = node_config(ALGO_PROBABILITY);
    follow.in_bus[0] = B_IN;
    follow.in_bus[2] = B_PASSED;                         // nei <- the lead's passed
    follow.out_bus[0] = 2;                               // a note bus of its own
    follow.params[P_CONDITION] = TrigCondition::COND_NOT_NEI;
    Probability second(follow);

    for (uint8_t i = 0; i < 6; i++) {
        bus.note_write(B_IN, on((uint8_t)(60 + i)));
        bus.swap();
        first.process(bus, 0);                           // writes B_PASSED
        publish(bus, B_PASSED, B_OUT);
        second.process(bus, 0);                          // reads it in the same pass
        bus.swap();
        const bool lead_played = bus.note_count(B_OUT) == 1;
        const bool follow_played = bus.note_count(2) == 1;
        TEST_ASSERT_EQUAL(i % 2 == 0, lead_played);
        // Exactly one of them plays each time: `not nei` is the complement.
        TEST_ASSERT_TRUE(lead_played != follow_played);
        TEST_ASSERT_EQUAL(lead_played, bus.gate_read(B_PASSED));

        bus.note_write(B_IN, off((uint8_t)(60 + i)));
        bus.swap();
        first.process(bus, 0);
        publish(bus, B_PASSED, B_OUT);
        second.process(bus, 0);
        bus.swap();
        // The latch still says what it said, with no event in between.
        TEST_ASSERT_EQUAL(lead_played, bus.gate_read(B_PASSED));
    }
}

// The rule is about notes. A CC or a bend crossing the node is not thinned
// and does not move the count under the notes.
static void test_other_events_pass_and_do_not_advance_the_count() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.params[P_RATIO] = 2;                               // 1:2
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

static void test_a_transport_start_restarts_the_note_count() {
    BusManager bus;
    NodeConfig c = probability_config();
    c.params[P_CONDITION] = TrigCondition::COND_FIRST;
    Probability node(c);

    bus.note_write(B_IN, on(60));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());
    bus.note_write(B_IN, on(61));
    TEST_ASSERT_EQUAL(0, note_pass(bus, node).size());

    node.transport_event(bus, TRANSPORT_CONTINUE);       // not a restart
    bus.note_write(B_IN, on(62));
    TEST_ASSERT_EQUAL(0, note_pass(bus, node).size());

    node.transport_event(bus, TRANSPORT_START);
    bus.note_write(B_IN, on(63));
    TEST_ASSERT_EQUAL(1, note_pass(bus, node).size());
}

// ---------------------------------------------------------------------------
// GateProbability: the gate side
// ---------------------------------------------------------------------------

static NodeConfig gate_probability_config() {
    NodeConfig c = node_config(ALGO_GATE_PROBABILITY);
    c.in_bus[0] = B_IN;
    c.out_bus[0] = B_OUT;
    c.out_bus[1] = B_PASSED;
    return c;
}

// The same rule, counted in rising edges rather than note-ons: a hat and a
// melody set to the same ratio fall on the same beats.
static void test_gate_ratio_counts_rising_edges() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.params[P_RATIO] = 9;                               // 3:4
    GateProbability node(c);

    for (uint8_t i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL(i % 4 == 2, gate_trigger(bus, node));
    }
}

// The decision is taken on the edge and held for the whole of the gate, so a
// passed gate is exactly as long as its input and a dropped one never
// flickers - the level equivalent of pairing a note-off with its note-on.
static void test_the_decision_is_held_for_the_length_of_the_gate() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.params[P_RATIO] = 2;                               // 1:2: pass, drop, pass...
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
    c.params[P_RATIO] = 3;                               // 2:2
    GateProbability node(c);

    for (uint8_t i = 0; i < 10; i++) {                   // held, never falls
        bus.gate_write(B_IN, true);
        bus.swap(); node.process(bus, 0); bus.swap();
        TEST_ASSERT_FALSE(bus.gate_read(B_OUT));         // still the 1st of 2
    }
    bus.swap(); node.process(bus, 0); bus.swap();        // falls
    TEST_ASSERT_TRUE(gate_trigger(bus, node));           // the 2nd of 2
}

// Both nodes read and write the same latch, so a gate pattern can answer a
// note pattern and the other way round.
static void test_a_gate_node_follows_a_note_node_through_passed() {
    BusManager bus;
    NodeConfig lead = probability_config();
    lead.params[P_RATIO] = 2;                            // 1:2
    Probability notes(lead);

    NodeConfig follow = gate_probability_config();
    follow.in_bus[2] = B_PASSED;
    follow.out_bus[1] = 7;                               // a latch of its own
    follow.params[P_CONDITION] = TrigCondition::COND_NEI;
    GateProbability gates(follow);

    for (uint8_t i = 0; i < 6; i++) {
        bus.note_write(B_IN, on((uint8_t)(60 + i)));
        bus.gate_write(B_IN, true);
        bus.swap();
        notes.process(bus, 0);
        publish(bus, B_PASSED, B_OUT);
        gates.process(bus, 0);
        bus.swap();
        TEST_ASSERT_EQUAL(i % 2 == 0, bus.note_count(B_OUT) == 1);
        TEST_ASSERT_EQUAL(i % 2 == 0, bus.gate_read(B_OUT));
        bus.swap();
        notes.process(bus, 0);
        publish(bus, B_PASSED, B_OUT);
        gates.process(bus, 0);
        bus.swap();
    }
}

static void test_a_gate_node_defaults_to_passing_everything() {
    BusManager bus;
    GateProbability node(gate_probability_config());     // params zeroed
    for (uint8_t i = 0; i < 10; i++) TEST_ASSERT_TRUE(gate_trigger(bus, node));
}

static void test_a_transport_start_restarts_the_gate_count() {
    BusManager bus;
    NodeConfig c = gate_probability_config();
    c.params[P_CONDITION] = TrigCondition::COND_FIRST;
    GateProbability node(c);

    TEST_ASSERT_TRUE(gate_trigger(bus, node));
    TEST_ASSERT_FALSE(gate_trigger(bus, node));
    node.transport_event(bus, TRANSPORT_START);
    TEST_ASSERT_TRUE(gate_trigger(bus, node));
}

// ---------------------------------------------------------------------------
// Both nodes, one block
// ---------------------------------------------------------------------------

// The point of the shared object: the same four parameters, in the same
// order, with the same descriptors, so a preset's bytes and an editor's
// controls mean the same thing on either node.
static void test_both_nodes_expose_one_block() {
    const AlgorithmDescriptor* notes = registry::find(ALGO_PROBABILITY);
    const AlgorithmDescriptor* gates = registry::find(ALGO_GATE_PROBABILITY);
    TEST_ASSERT_NOT_NULL(notes);
    TEST_ASSERT_NOT_NULL(gates);
    TEST_ASSERT_EQUAL(TrigCondition::N_PARAMS, notes->n_params);
    TEST_ASSERT_EQUAL(TrigCondition::N_PARAMS, gates->n_params);
    for (uint16_t i = 0; i < TrigCondition::N_PARAMS; i++) {
        const ParamDescriptor* a = registry::param(*notes, i);
        const ParamDescriptor* b = registry::param(*gates, i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_PTR(a, b);                     // literally one table
    }
    // The neighbour inlet and the latch outlet are on both, by the same name.
    TEST_ASSERT_EQUAL_STRING("fill", notes->in_name[1]);
    TEST_ASSERT_EQUAL_STRING("nei", notes->in_name[2]);
    TEST_ASSERT_EQUAL_STRING("passed", notes->out_name[1]);
    TEST_ASSERT_EQUAL_STRING("fill", gates->in_name[1]);
    TEST_ASSERT_EQUAL_STRING("nei", gates->in_name[2]);
    TEST_ASSERT_EQUAL_STRING("passed", gates->out_name[1]);
    // Only the signal inlet is required; fill and nei are optional.
    TEST_ASSERT_EQUAL(1, notes->min_in);
    TEST_ASSERT_EQUAL(1, gates->min_in);
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
    b.out_bus[0] = 5;
    b.out_bus[1] = 6;
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
    RUN_TEST(test_the_ratio_enum_is_the_list_it_names);
    RUN_TEST(test_the_condition_block_describes_itself);
    RUN_TEST(test_a_ratio_passes_the_xth_of_every_y);
    RUN_TEST(test_the_count_wraps_without_moving_the_ratio);
    RUN_TEST(test_first_and_not_first);
    RUN_TEST(test_pre_follows_the_last_decision);
    RUN_TEST(test_fill_and_nei_come_from_the_inlets);
    RUN_TEST(test_the_three_controls_and_together);
    RUN_TEST(test_the_count_is_of_events_not_of_passes);
    RUN_TEST(test_a_restart_puts_the_count_back_on_the_downbeat);
    RUN_TEST(test_the_seed_reseeds_and_the_rest_moves_freely);

    RUN_TEST(test_a_ratio_thins_notes_and_keeps_their_note_offs);
    RUN_TEST(test_the_fill_inlet_arms_the_condition);
    RUN_TEST(test_passed_is_latched_and_drives_a_neighbour);
    RUN_TEST(test_other_events_pass_and_do_not_advance_the_count);
    RUN_TEST(test_a_transport_start_restarts_the_note_count);

    RUN_TEST(test_gate_ratio_counts_rising_edges);
    RUN_TEST(test_the_decision_is_held_for_the_length_of_the_gate);
    RUN_TEST(test_a_held_gate_is_one_event);
    RUN_TEST(test_a_gate_node_follows_a_note_node_through_passed);
    RUN_TEST(test_a_gate_node_defaults_to_passing_everything);
    RUN_TEST(test_a_transport_start_restarts_the_gate_count);

    RUN_TEST(test_both_nodes_expose_one_block);
    RUN_TEST(test_two_nodes_at_the_same_odds_can_be_made_to_disagree);
    return UNITY_END();
}
