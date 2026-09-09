#include <unity.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "master.h"
#include "midi/note_event.h"
#include "algorithm/sequencer/drum_sequencer.h"

void setUp() {}
void tearDown() {}

typedef DrumSequencer DS;

static const uint8_t ADVANCE = 0, RESET = 1, NOTE_OUT = 3;
static const uint32_t HALF_US = 20000;

// Gate outlets: lane l on gate bus 2 + l.
static uint8_t lane_bus(uint8_t lane) { return (uint8_t)(2 + lane); }

struct Rig {
    BusManager bus;
    uint32_t now;
    Rig() : bus(), now(0) {}

    std::vector<MidiEvent> notes() {
        std::vector<MidiEvent> out;
        const uint8_t n = bus.note_count(NOTE_OUT);
        for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(NOTE_OUT, i));
        return out;
    }
    uint8_t gates() {
        uint8_t mask = 0;
        for (uint8_t l = 0; l < DS::LANES; l++) if (bus.gate_read(lane_bus(l))) mask |= (uint8_t)(1u << l);
        return mask;
    }
    void pass(Node& node, uint32_t dt = HALF_US) {
        now += dt;
        bus.swap(); node.process(bus, now); bus.swap();
    }
    // One advance edge; returns the lanes whose gates went high on it.
    uint8_t edge(Node& node) {
        bus.gate_write(ADVANCE, true);
        bus.swap(); node.process(bus, now); bus.swap();
        const uint8_t fired = gates();
        now += HALF_US;
        pass(node);
        return fired;
    }
    void reset_pulse(Node& node) {
        bus.gate_write(RESET, true);
        bus.swap(); node.process(bus, now); bus.swap();
        pass(node);
    }
};

static NodeConfig gate_config(uint8_t length) {
    NodeConfig c = node_config(ALGO_DRUM_SEQ_GATE);
    c.in_bus[0] = ADVANCE;
    for (uint8_t l = 0; l < DS::LANES; l++) c.out_bus[l] = lane_bus(l);
    c.params[DS::P_LENGTH] = length;
    return c;
}

static void gate_lane(NodeConfig& c, uint8_t lane, uint32_t bits, uint8_t length = 0, uint8_t probability = 0) {
    uint8_t* d = &c.params[DS::LANE_BASE + lane * DS::LANE_STRIDE];
    d[0] = (uint8_t)bits; d[1] = (uint8_t)(bits >> 8); d[2] = (uint8_t)(bits >> 16); d[3] = (uint8_t)(bits >> 24);
    d[4] = length;
    d[5] = probability;
}

static NodeConfig midi_config(uint8_t length) {
    NodeConfig c = node_config(ALGO_DRUM_SEQ_MIDI);
    c.in_bus[0] = ADVANCE;
    c.out_bus[0] = NOTE_OUT;
    c.params[DS::P_LENGTH] = length;
    return c;
}

static void midi_lane(NodeConfig& c, uint8_t lane, uint8_t note, uint8_t channel, uint8_t length = 0, uint8_t probability = 0) {
    uint8_t* d = &c.params[DS::LANE_BASE + lane * DS::LANE_STRIDE];
    d[0] = note; d[1] = channel; d[2] = length; d[3] = probability;
}

static void midi_cell(NodeConfig& c, uint8_t lane, uint8_t step, uint8_t velocity) {
    c.params[DrumSeqMidi::VELOCITY_BASE + lane * MAX_SEQUENCE_LEN + step] = velocity;
}

// A pattern string per lane, 'x' for a hit, over one cycle of edges.
static std::string lane_string(const std::vector<uint8_t>& fired, uint8_t lane) {
    std::string s;
    for (uint8_t f : fired) s += (f & (1u << lane)) ? 'x' : '.';
    return s;
}

// ---------------------------------------------------------------------------
// The gate variant
// ---------------------------------------------------------------------------

// An 8-lane pattern produces the expected hits on the expected lanes over two
// full cycles.
static void test_eight_lanes_to_gates_over_two_cycles() {
    NodeConfig c = gate_config(8);
    const uint32_t patterns[8] = {0b00010001, 0b00010000, 0b01010101, 0b10101010, 0b00000001, 0b10000000, 0b11111111, 0b00000000};
    for (uint8_t l = 0; l < 8; l++) gate_lane(c, l, patterns[l]);
    DrumSeqGate node(c);
    Rig rig;
    std::vector<uint8_t> fired;
    for (uint8_t i = 0; i < 16; i++) fired.push_back(rig.edge(node));
    TEST_ASSERT_EQUAL_STRING("x...x...x...x...", lane_string(fired, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("....x.......x...", lane_string(fired, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("x.x.x.x.x.x.x.x.", lane_string(fired, 2).c_str());
    TEST_ASSERT_EQUAL_STRING(".x.x.x.x.x.x.x.x", lane_string(fired, 3).c_str());
    TEST_ASSERT_EQUAL_STRING("x.......x.......", lane_string(fired, 4).c_str());
    TEST_ASSERT_EQUAL_STRING(".......x.......x", lane_string(fired, 5).c_str());
    TEST_ASSERT_EQUAL_STRING("xxxxxxxxxxxxxxxx", lane_string(fired, 6).c_str());
    TEST_ASSERT_EQUAL_STRING("................", lane_string(fired, 7).c_str());
    for (uint8_t l = 0; l < 8; l++) TEST_ASSERT_EQUAL_UINT32(patterns[l], node.pattern(l));
}

// Per-lane lengths produce a polyrhythm: lanes of 16 and 12 steps realign
// after 48 advances and not before.
static void test_polyrhythm_16_against_12_realigns_after_48() {
    NodeConfig c = gate_config(16);
    gate_lane(c, 0, 1, 16);                                     // step 0 only, 16 steps
    gate_lane(c, 1, 1, 12);                                     // step 0 only, 12 steps
    DrumSeqGate node(c);
    TEST_ASSERT_EQUAL(16, node.lane_length(0));
    TEST_ASSERT_EQUAL(12, node.lane_length(1));
    Rig rig;
    std::vector<uint8_t> fired;
    for (uint8_t i = 0; i < 97; i++) fired.push_back(rig.edge(node));
    for (uint8_t i = 0; i < 97; i++) {
        const bool both = (fired[i] & 3) == 3;
        TEST_ASSERT_EQUAL_MESSAGE((i % 48) == 0, both, "the two lanes coincide every 48 edges and nowhere else");
        TEST_ASSERT_EQUAL((i % 16) == 0, (fired[i] & 1) != 0);
        TEST_ASSERT_EQUAL((i % 12) == 0, (fired[i] & 2) != 0);
    }
}

// Triggers are of the configured fixed width, and accent is a lane like any
// other: lane 1 marks which of lane 0's hits are accented.
static void test_gate_trigger_width_and_accent_lane() {
    NodeConfig c = gate_config(4);
    c.params[DS::P_GATE] = 8;                                   // 8 ms
    gate_lane(c, 0, 0b1111);                                    // every step
    gate_lane(c, 1, 0b0001);                                    // accent on the downbeat
    DrumSeqGate node(c);
    Rig rig;
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL(0b11, rig.gates());
    rig.pass(node, 7000);
    TEST_ASSERT_EQUAL(0b11, rig.gates());                        // 7 ms: still high
    rig.pass(node, 2000);
    TEST_ASSERT_EQUAL(0, rig.gates());                           // 9 ms: down
    rig.now += 11000;
    TEST_ASSERT_EQUAL(0b01, rig.edge(node));                     // step 1: no accent
}

static void test_per_lane_probability() {
    NodeConfig c = gate_config(1);
    gate_lane(c, 0, 1, 0, 100);
    gate_lane(c, 1, 1, 0, 1);
    DrumSeqGate node(c);
    Rig rig;
    uint16_t certain = 0, unlikely = 0;
    for (uint16_t i = 0; i < 200; i++) { const uint8_t f = rig.edge(node); if (f & 1) certain++; if (f & 2) unlikely++; }
    TEST_ASSERT_EQUAL(200, certain);
    TEST_ASSERT_TRUE(unlikely < 20);
}

// A reset pulse returns every lane to step 0, including lanes of differing
// length.
static void test_reset_returns_every_lane_to_step_zero() {
    NodeConfig c = gate_config(16);
    c.in_bus[1] = RESET;
    gate_lane(c, 0, 1, 16);
    gate_lane(c, 1, 1, 12);
    gate_lane(c, 2, 1, 5);
    DrumSeqGate node(c);
    Rig rig;
    for (uint8_t i = 0; i < 7; i++) rig.edge(node);
    TEST_ASSERT_EQUAL(6, node.lane_position(0));
    TEST_ASSERT_EQUAL(6, node.lane_position(1));
    TEST_ASSERT_EQUAL(1, node.lane_position(2));
    rig.reset_pulse(node);
    TEST_ASSERT_EQUAL(0b111, rig.edge(node));
    for (uint8_t l = 0; l < 3; l++) TEST_ASSERT_EQUAL(0, node.lane_position(l));
}

// Lanes without a jack are left unconnected, and that is not an error.
static void test_unconnected_lanes_are_accepted() {
    NodeConfig c = gate_config(8);
    for (uint8_t l = 3; l < DS::LANES; l++) c.out_bus[l] = NO_BUS;
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
    c.out_bus[2] = N_GATE_BUS;
    TEST_ASSERT_EQUAL(CONFIG_OUTLET_OUT_OF_RANGE, registry::validate(c));
    c.out_bus[2] = NO_BUS;
    gate_lane(c, 5, 0xFF);                                      // fires into nothing, harmlessly
    DrumSeqGate node(c);
    Rig rig;
    for (uint8_t i = 0; i < 8; i++) TEST_ASSERT_EQUAL(0, rig.edge(node) & 0xF8);
}

// ---------------------------------------------------------------------------
// The MIDI variant
// ---------------------------------------------------------------------------

static std::string describe(const std::vector<MidiEvent>& events) {
    std::string s;
    for (const MidiEvent& e : events) {
        char buf[32];
        snprintf(buf, sizeof buf, "%s%u/%u@%u ", is_note_on(e) ? "+" : "-", e.data1, e.data2, e.channel);
        s += buf;
    }
    return s;
}

// The configured note number per lane, per-step velocity, and a matching
// note-off for every note-on after the gate time.
static void test_midi_lanes_emit_their_note_and_velocity_and_release() {
    NodeConfig c = midi_config(4);
    c.params[DS::P_GATE] = 15;                                  // 15 ms
    midi_lane(c, 0, 36, 10);
    midi_lane(c, 1, 38, 10);
    midi_lane(c, 2, 0, 0);                                      // defaults: GM closed hat, channel 10
    midi_lane(c, 3, 60, 3);
    midi_cell(c, 0, 0, 127); midi_cell(c, 0, 2, 90);
    midi_cell(c, 1, 2, 100);
    midi_cell(c, 2, 0, 40); midi_cell(c, 2, 1, 40); midi_cell(c, 2, 2, 40); midi_cell(c, 2, 3, 40);
    midi_cell(c, 3, 3, 64);
    DrumSeqMidi node(c);
    TEST_ASSERT_EQUAL(42, node.lane_note(2));
    TEST_ASSERT_EQUAL(10, node.lane_channel(2));
    Rig rig;
    // Step 0
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL_STRING("+36/127@10 +42/40@10 ", describe(rig.notes()).c_str());
    rig.pass(node, 10000);
    TEST_ASSERT_EQUAL_STRING("", describe(rig.notes()).c_str());
    rig.pass(node, 6000);
    TEST_ASSERT_EQUAL_STRING("-36/0@10 -42/0@10 ", describe(rig.notes()).c_str());
    TEST_ASSERT_EQUAL(0, node.sounding_count());
    rig.now += 24000;
    // Steps 1, 2, 3
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL_STRING("+42/40@10 ", describe(rig.notes()).c_str());
    rig.pass(node, 40000);
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL_STRING("+36/90@10 +38/100@10 +42/40@10 ", describe(rig.notes()).c_str());
    rig.pass(node, 40000);
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL_STRING("+42/40@10 +60/64@3 ", describe(rig.notes()).c_str());
}

// A lane retriggered before its note has been released releases it first:
// the ledger never holds two notes for one lane.
static void test_retrigger_before_release_releases_first() {
    NodeConfig c = midi_config(2);
    c.params[DS::P_GATE] = 200;                                 // longer than a step
    midi_lane(c, 0, 36, 10);
    midi_cell(c, 0, 0, 100); midi_cell(c, 0, 1, 100);
    DrumSeqMidi node(c);
    Rig rig;
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL_STRING("+36/100@10 ", describe(rig.notes()).c_str());
    rig.pass(node, 40000);
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    TEST_ASSERT_EQUAL_STRING("-36/0@10 +36/100@10 ", describe(rig.notes()).c_str());
    TEST_ASSERT_EQUAL(1, node.sounding_count());
}

// Every note-on gets its note-off: across the whole grid at random, with
// irregular timing, and then silence.
static void test_midi_hangs_nothing() {
    NodeConfig c = midi_config(16);
    c.in_bus[1] = RESET;
    c.params[DS::P_GATE] = 30;
    Xorshift32 script(77);
    for (uint8_t l = 0; l < DS::LANES; l++) {
        midi_lane(c, l, (uint8_t)(35 + l), (uint8_t)(1 + script.below(16)), (uint8_t)(1 + script.below(MAX_SEQUENCE_LEN)), (uint8_t)script.below(101));
        for (uint8_t s = 0; s < MAX_SEQUENCE_LEN; s++) midi_cell(c, l, s, script.chance(50) ? (uint8_t)(1 + script.below(127)) : 0);
    }
    DrumSeqMidi node(c);
    Rig rig;
    int16_t balance[16][128] = {};
    bool negative = false;
    for (uint16_t i = 0; i < 1500; i++) {
        if (script.chance(5)) rig.bus.gate_write(RESET, true);
        rig.bus.gate_write(ADVANCE, true);
        rig.now += script.below(60) * 1000u;
        rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
        for (const MidiEvent& e : rig.notes()) {
            int16_t& b = balance[(e.channel - 1) & 15][e.data1];
            if (is_note_on(e)) b++; else if (is_note_off(e)) { b--; if (b < 0) negative = true; }
        }
        TEST_ASSERT_FALSE(negative);
        TEST_ASSERT_EQUAL(0, rig.bus.note_overflows(NOTE_OUT));
        rig.pass(node, script.below(40) * 1000u);
        for (const MidiEvent& e : rig.notes()) {
            int16_t& b = balance[(e.channel - 1) & 15][e.data1];
            if (is_note_on(e)) b++; else if (is_note_off(e)) { b--; if (b < 0) negative = true; }
        }
    }
    rig.pass(node, 100000);
    for (const MidiEvent& e : rig.notes()) {
        int16_t& b = balance[(e.channel - 1) & 15][e.data1];
        if (is_note_on(e)) b++; else if (is_note_off(e)) { b--; if (b < 0) negative = true; }
    }
    TEST_ASSERT_FALSE(negative);
    uint16_t total = 0;
    for (uint8_t ch = 0; ch < 16; ch++) for (uint8_t n = 0; n < 128; n++) if (balance[ch][n] > 0) total = (uint16_t)(total + balance[ch][n]);
    TEST_ASSERT_EQUAL(0, total);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// A patch swap mid-note: the master calls silence() and flushes the
// note-offs through the old patch's MIDI port before destroying it.
static void test_patch_swap_mid_note_releases_every_lane() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);                    // tick -> gate 0
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 6;
    p.nodes[1] = midi_config(4);
    p.nodes[1].params[DS::P_GATE] = 250;                         // long enough to be caught mid-note
    midi_lane(p.nodes[1], 0, 36, 10);
    midi_lane(p.nodes[1], 1, 38, 10);
    midi_cell(p.nodes[1], 0, 0, 100);
    midi_cell(p.nodes[1], 1, 0, 100);
    p.n_nodes = 2;
    p.midi_out[0] = MidiOutConfig{mmMIDI_SERIAL_1, 0, NOTE_OUT};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    uint32_t now = 0;
    for (uint32_t t = 0; t < 8 * CLOCK_SUBTICK; t++) { master.clock().advance(); master.pass(now); now += 300; }
    for (int i = 0; i < 4; i++) { master.pass(now); now += 300; }
    TEST_ASSERT_EQUAL(2, midi.messages.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, midi.messages[0].type);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(empty_patch()));
    TEST_ASSERT_EQUAL(4, midi.messages.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, midi.messages[2].type);
    TEST_ASSERT_EQUAL(36, midi.messages[2].d1);
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, midi.messages[3].type);
    TEST_ASSERT_EQUAL(38, midi.messages[3].d1);
    TEST_ASSERT_EQUAL(mmMIDI_SERIAL_1, midi.messages[3].target);
}

// The gate variant through the master, on jacks, driven by a divider: the
// lanes reach the jacks with the trigger width from #4.
static void test_gate_variant_drives_jacks_from_a_divider() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 6;
    p.nodes[1] = gate_config(4);
    gate_lane(p.nodes[1], 0, 0b0101);
    gate_lane(p.nodes[1], 1, 0b1010);
    for (uint8_t l = 2; l < DS::LANES; l++) p.nodes[1].out_bus[l] = NO_BUS;
    p.n_nodes = 2;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, lane_bus(0)};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, lane_bus(1)};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    uint32_t now = 0, rises0 = 0, rises1 = 0;
    bool was0 = false, was1 = false;
    for (uint32_t t = 0; t < 8 * 6 * CLOCK_SUBTICK; t++) {
        master.clock().advance();
        master.pass(now);
        const bool h0 = gpio.outputs[0] == GPIO_HIGH, h1 = gpio.outputs[1] == GPIO_HIGH;
        if (h0 && !was0) rises0++;
        if (h1 && !was1) rises1++;
        TEST_ASSERT_FALSE(h0 && h1);                             // never on the same step
        was0 = h0; was1 = h1;
        now += 300;
    }
    for (int i = 0; i < 4; i++) { master.pass(now); now += 300; if (gpio.outputs[1] == GPIO_HIGH && !was1) rises1++; was1 = gpio.outputs[1] == GPIO_HIGH; }
    TEST_ASSERT_EQUAL_UINT32(4, rises0);
    TEST_ASSERT_EQUAL_UINT32(4, rises1);
}

static void test_descriptors_and_sizes() {
    TEST_ASSERT_EQUAL(DS::LANES, DrumSeqGate::descriptor.n_out);
    TEST_ASSERT_EQUAL(1, DrumSeqMidi::descriptor.n_out);
    TEST_ASSERT_EQUAL(DrumSeqGate::PARAM_COUNT, DrumSeqGate::descriptor.n_params);
    TEST_ASSERT_EQUAL(DrumSeqMidi::PARAM_COUNT, DrumSeqMidi::descriptor.n_params);
    TEST_ASSERT_TRUE(DrumSeqMidi::descriptor.n_params <= N_PARAM);
    TEST_ASSERT_TRUE(sizeof(DrumSeqGate) <= NODE_SLOT_SIZE);
    TEST_ASSERT_TRUE(sizeof(DrumSeqMidi) <= NODE_SLOT_SIZE);
    TEST_ASSERT_TRUE(sizeof(DrumSeqGate) < sizeof(DrumSeqMidi) / 2);   // no velocity to store
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_eight_lanes_to_gates_over_two_cycles);
    RUN_TEST(test_polyrhythm_16_against_12_realigns_after_48);
    RUN_TEST(test_gate_trigger_width_and_accent_lane);
    RUN_TEST(test_per_lane_probability);
    RUN_TEST(test_reset_returns_every_lane_to_step_zero);
    RUN_TEST(test_unconnected_lanes_are_accepted);
    RUN_TEST(test_midi_lanes_emit_their_note_and_velocity_and_release);
    RUN_TEST(test_retrigger_before_release_releases_first);
    RUN_TEST(test_midi_hangs_nothing);
    RUN_TEST(test_patch_swap_mid_note_releases_every_lane);
    RUN_TEST(test_gate_variant_drives_jacks_from_a_divider);
    RUN_TEST(test_descriptors_and_sizes);
    return UNITY_END();
}
