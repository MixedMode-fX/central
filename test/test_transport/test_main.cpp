#include <unity.h>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "algorithm/clock/transport.h"
#include "algorithm/sequencer/sequencers.h"
#include "master.h"
#include "hal/midi_types.h"

void setUp() {}
void tearDown() {}

static NodeConfig transport_config(uint8_t start_bus, uint8_t stop_bus, uint8_t continue_bus) {
    NodeConfig c = node_config(ALGO_TRANSPORT);
    c.out_bus[Transport::OUT_START] = start_bus;
    c.out_bus[Transport::OUT_STOP] = stop_bus;
    c.out_bus[Transport::OUT_CONTINUE] = continue_bus;
    return c;
}

// One Transport with all three outlets patched, each out to a jack, so what
// the node did is read off the pins rather than out of the node.
static Patch three_jacks_patch() {
    Patch p = empty_patch();
    p.nodes[0] = transport_config(0, 1, 2);
    p.n_nodes = 1;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 0};      // jack 1: start
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 1};      // jack 2: stop
    p.gate_ports[2] = GatePortConfig{GATE_PORT_OUT, 2};      // jack 3: continue
    return p;
}

// Rising edges on those three jacks, counted across a run of passes one
// millisecond apart - long enough for a 5 ms trigger to span a few of them.
struct Rises { uint32_t start; uint32_t stop; uint32_t cont; };

static void run(MixedModeMaster& master, FakeGpio& gpio, uint32_t passes,
                uint32_t& now, Rises& r, bool was[3]) {
    for (uint32_t i = 0; i < passes; i++) {
        master.pass(now);
        const bool high[3] = { gpio.outputs[0] == GPIO_HIGH,
                               gpio.outputs[1] == GPIO_HIGH,
                               gpio.outputs[2] == GPIO_HIGH };
        if (high[0] && !was[0]) r.start++;
        if (high[1] && !was[1]) r.stop++;
        if (high[2] && !was[2]) r.cont++;
        for (uint8_t j = 0; j < 3; j++) was[j] = high[j];
        now += 1000;
    }
}

// ---------------------------------------------------------------------------
// The three messages
// ---------------------------------------------------------------------------

// Each message fires its own outlet and only its own. Moving the transport
// from the console - which is what calling the clock directly is - reaches
// the patch exactly as a MIDI byte does: the transport is the module's.
static void test_each_message_fires_its_own_outlet() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(three_jacks_patch()));
    master.setup();

    uint32_t now = 0;
    Rises r = {0, 0, 0};
    bool was[3] = {false, false, false};
    run(master, gpio, 20, now, r, was);              // an idle transport says nothing
    TEST_ASSERT_EQUAL_UINT32(0, r.start + r.stop + r.cont);

    master.clock().start();
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.start);
    TEST_ASSERT_EQUAL_UINT32(0, r.stop + r.cont);

    master.clock().stop();
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.stop);
    TEST_ASSERT_EQUAL_UINT32(1, r.start + r.cont);

    master.clock().resume();
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.cont);
    TEST_ASSERT_EQUAL_UINT32(2, r.start + r.stop);
}

// The path the node exists for: a realtime byte on any MIDI input reaches the
// clock - no MidiInPort accepts it, it is not note traffic - and comes back
// out of the patch as a trigger.
static void test_midi_realtime_bytes_fire_the_outlets() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(three_jacks_patch()));
    master.setup();

    uint32_t now = 0;
    Rises r = {0, 0, 0};
    bool was[3] = {false, false, false};

    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_START, 0, 0, 0}, now));
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.start);

    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_STOP, 0, 0, 0}, now));
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.stop);

    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_HOST_1, MidiEvent{MIDI_CONTINUE, 0, 0, 0}, now));
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.cont);

    // A clock byte is the transport running, not the transport moving.
    for (uint8_t i = 0; i < 8; i++) {
        master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_CLOCK, 0, 0, 0}, now);
        run(master, gpio, 2, now, r, was);
    }
    TEST_ASSERT_EQUAL_UINT32(3, r.start + r.stop + r.cont);
}

// A message the clock was already in the state of is still a message: a DAW
// that sends stop twice has said it twice, and a start under a running clock
// is a re-sync - the moment a patch most wants putting back on step one.
static void test_a_repeated_message_fires_again() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(three_jacks_patch()));
    master.setup();

    uint32_t now = 0;
    Rises r = {0, 0, 0};
    bool was[3] = {false, false, false};
    for (uint8_t i = 0; i < 3; i++) {
        master.clock().start();
        run(master, gpio, 20, now, r, was);
        master.clock().stop();
        run(master, gpio, 20, now, r, was);
        master.clock().stop();
        run(master, gpio, 20, now, r, was);
    }
    TEST_ASSERT_EQUAL_UINT32(3, r.start);
    TEST_ASSERT_EQUAL_UINT32(6, r.stop);
}

// Two messages between two passes both fire. Their order is lost and nothing
// downstream can hear the difference: they are a millisecond apart.
static void test_two_messages_in_one_pass_both_fire() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(three_jacks_patch()));
    master.setup();

    uint32_t now = 0;
    Rises r = {0, 0, 0};
    bool was[3] = {false, false, false};
    master.clock().stop();
    master.clock().start();
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.start);
    TEST_ASSERT_EQUAL_UINT32(1, r.stop);
}

// ---------------------------------------------------------------------------
// What comes out of the outlet
// ---------------------------------------------------------------------------

// One message is one trigger, however many passes run afterwards, and it is a
// fixed width rather than a level that follows the transport.
static void test_one_message_is_one_fixed_width_trigger() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(three_jacks_patch()));
    master.setup();

    master.clock().start();
    uint32_t now = 0;
    uint32_t high_us = 0;
    uint32_t rises = 0;
    bool was = false;
    for (uint32_t i = 0; i < 200; i++) {             // 200 ms, far past the pulse
        master.pass(now);
        const bool high = gpio.outputs[0] == GPIO_HIGH;
        if (high && !was) rises++;
        if (high) high_us += 1000;
        was = high;
        now += 1000;
    }
    TEST_ASSERT_EQUAL_UINT32(1, rises);
    TEST_ASSERT_UINT32_WITHIN(2000, TRIGGER_WIDTH_US, high_us);
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[0]);
}

// The width is a parameter, and it moves at runtime like every other.
static void test_the_width_parameter_sets_the_trigger_length() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = three_jacks_patch();
    p.nodes[0].params[0] = 40;                       // 40 ms
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    uint32_t high_us = 0;
    master.clock().start();
    for (uint32_t i = 0; i < 200; i++) {
        master.pass(now);
        if (gpio.outputs[0] == GPIO_HIGH) high_us += 1000;
        now += 1000;
    }
    TEST_ASSERT_UINT32_WITHIN(2000, 40000, high_us);

    TEST_ASSERT_EQUAL(PARAM_SET_OK, master.set_node_param(0, 0, 10));
    uint8_t value = 0;
    TEST_ASSERT_TRUE(master.get_node_param(0, 0, value));
    TEST_ASSERT_EQUAL_UINT8(10, value);

    high_us = 0;
    master.clock().start();
    for (uint32_t i = 0; i < 200; i++) {
        master.pass(now);
        if (gpio.outputs[0] == GPIO_HIGH) high_us += 1000;
        now += 1000;
    }
    TEST_ASSERT_UINT32_WITHIN(2000, 10000, high_us);
}

// A gate bus is the OR of its writers, so two outlets on one bus is "either
// of them" with no logic node in between - which is how a patch asks to be
// reset whenever the transport rolls, start or continue.
static void test_two_outlets_on_one_bus_are_or() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = transport_config(0, NO_BUS, 0);     // start and continue, one bus
    p.n_nodes = 1;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    uint32_t rises = 0;
    bool was = false;
    for (uint8_t round = 0; round < 3; round++) {
        master.clock().start();
        for (uint32_t i = 0; i < 20; i++) {
            master.pass(now);
            const bool high = gpio.outputs[0] == GPIO_HIGH;
            if (high && !was) rises++;
            was = high;
            now += 1000;
        }
        master.clock().stop();                       // nothing reaches this bus
        master.clock().resume();
        for (uint32_t i = 0; i < 20; i++) {
            master.pass(now);
            const bool high = gpio.outputs[0] == GPIO_HIGH;
            if (high && !was) rises++;
            was = high;
            now += 1000;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(6, rises);
}

// Every outlet is optional, and a node with none of them patched is legal:
// nothing is written and nothing goes wrong.
static void test_outlets_are_optional() {
    NodeConfig c = node_config(ALGO_TRANSPORT);
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));

    BusManager bus;
    Transport node(c);
    node.process(bus, 0);
    node.transport_event(bus, TRANSPORT_START | TRANSPORT_STOP | TRANSPORT_CONTINUE);
    bus.swap();
    TEST_ASSERT_EQUAL_UINT32(1, node.fired(Transport::OUT_START));
    TEST_ASSERT_EQUAL_UINT32(1, node.fired(Transport::OUT_STOP));
    TEST_ASSERT_EQUAL_UINT32(1, node.fired(Transport::OUT_CONTINUE));
    for (uint8_t b = 0; b < N_GATE_BUS; b++) TEST_ASSERT_FALSE(bus.gate_read(b));
}

// The node has no inlets at all: what it reads is not on a bus.
static void test_the_node_has_no_inlets() {
    const AlgorithmDescriptor* d = registry::find(ALGO_TRANSPORT);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(0, d->n_in);
    TEST_ASSERT_EQUAL_UINT8(0, d->min_in);
    TEST_ASSERT_EQUAL_UINT8(Transport::OUTLETS, d->n_out);
    TEST_ASSERT_FALSE(d->wants_tick);
}

// ---------------------------------------------------------------------------
// What it is for
// ---------------------------------------------------------------------------

// The headline: a start puts a running sequencer back on its first step, in
// the pass the start arrived in. A sequencer counts edges rather than
// subticks, so the clock's own count going back to zero does nothing for it.
static void test_a_start_resets_a_running_sequencer() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};       // jack 1 advances it
    p.nodes[0] = transport_config(1, NO_BUS, NO_BUS);        // start -> gate 1
    p.nodes[1] = node_config(ALGO_STEP_SEQ);
    p.nodes[1].in_bus[0] = 0;                                // advance
    p.nodes[1].in_bus[1] = 1;                                // reset, from the start outlet
    p.nodes[1].out_bus[0] = 2;
    p.nodes[1].params[0] = 8;                                // eight steps
    p.n_nodes = 2;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    const StepSequencer* seq = static_cast<const StepSequencer*>(master.node(1));
    TEST_ASSERT_NOT_NULL(seq);

    uint32_t now = 0;
    // An edge on the jack, and the passes that carry it through the graph.
    struct Advance {
        static void edge(MixedModeMaster& m, FakeGpio& g, uint32_t& t) {
            g.set_input(0, GPIO_HIGH);
            for (uint8_t i = 0; i < 3; i++) { m.pass(t); t += 1000; }
            g.set_input(0, GPIO_LOW);
            for (uint8_t i = 0; i < 3; i++) { m.pass(t); t += 1000; }
        }
    };

    for (uint8_t e = 0; e < 5; e++) Advance::edge(master, gpio, now);
    TEST_ASSERT_EQUAL_UINT8(4, seq->position());             // five steps in

    master.clock().start();
    master.pass(now); now += 1000;                           // the reset lands here
    Advance::edge(master, gpio, now);
    TEST_ASSERT_EQUAL_UINT8(0, seq->position());             // back at the top

    // And without a start it carries on from there, so it was the transport
    // that reset it and not the passing of time.
    Advance::edge(master, gpio, now);
    TEST_ASSERT_EQUAL_UINT8(1, seq->position());
}

// The edge belongs to the patch that was loaded when it happened. A patch
// loaded with one in the air is not late for it: it was not there.
static void test_a_pending_edge_does_not_carry_across_a_load() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(empty_patch()));
    master.setup();

    master.clock().start();                                  // nothing in the patch hears it
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(three_jacks_patch()));
    master.setup();

    uint32_t now = 0;
    Rises r = {0, 0, 0};
    bool was[3] = {false, false, false};
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(0, r.start + r.stop + r.cont);

    // The next one is heard, so nothing has been latched shut.
    master.clock().start();
    run(master, gpio, 20, now, r, was);
    TEST_ASSERT_EQUAL_UINT32(1, r.start);
}

// Two Transports in one patch both hear the same edge: the transport is the
// module's, and one node reading it does not consume it.
static void test_two_transports_both_hear_it() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = transport_config(0, NO_BUS, NO_BUS);
    p.nodes[1] = transport_config(1, NO_BUS, NO_BUS);
    p.n_nodes = 2;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 0};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 1};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.clock().start();
    master.pass(0);
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[0]);
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[1]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_each_message_fires_its_own_outlet);
    RUN_TEST(test_midi_realtime_bytes_fire_the_outlets);
    RUN_TEST(test_a_repeated_message_fires_again);
    RUN_TEST(test_two_messages_in_one_pass_both_fire);
    RUN_TEST(test_one_message_is_one_fixed_width_trigger);
    RUN_TEST(test_the_width_parameter_sets_the_trigger_length);
    RUN_TEST(test_two_outlets_on_one_bus_are_or);
    RUN_TEST(test_outlets_are_optional);
    RUN_TEST(test_the_node_has_no_inlets);
    RUN_TEST(test_a_start_resets_a_running_sequencer);
    RUN_TEST(test_a_pending_edge_does_not_carry_across_a_load);
    RUN_TEST(test_two_transports_both_hear_it);
    return UNITY_END();
}
