#include <unity.h>
#include <stdlib.h>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "hal/midi_types.h"

// Heap instrumentation: every operator new is counted, so the tests can
// assert that the firmware allocates nothing after setup().
static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

static void run_passes(MixedModeMaster& m, int n, uint32_t& now, uint32_t step_us = 1000) {
    for (int i = 0; i < n; i++) { m.pass(now); now += step_us; }
}

static NodeConfig node(uint8_t id, uint8_t in0, uint8_t out0) {
    NodeConfig c = node_config(id);
    c.in_bus[0] = in0;
    c.out_bus[0] = out0;
    return c;
}

// ---------------------------------------------------------------------------
// Gate in -> LogicNot -> GateToNote -> MidiOutPort: toggling the fake input
// produces exactly one MIDI message, with no algorithm touching a pin or a
// transport.
// ---------------------------------------------------------------------------
static void test_gate_in_through_algorithms_to_midi_out() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};                    // jack 1 -> gate bus 0
    p.nodes[0] = node(ALGO_LOGIC_NOT, 0, 1);                              // gate 0 -> gate 1
    p.nodes[1] = node(ALGO_GATE_TO_NOTE, 1, 0);                           // gate 1 -> note 0
    p.n_nodes = 2;
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 0};                    // note 0 -> USB cable 0
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT_PULLUP, gpio.modes[0]);

    uint32_t now = 0;
    gpio.set_input(0, GPIO_LOW);
    run_passes(master, 10, now);            // settle: NOT is high, GateToNote sent its note on
    midi.clear();

    gpio.set_input(0, GPIO_HIGH);           // toggle
    run_passes(master, 10, now);
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(mmMIDI_USB_0, midi.messages[0].target);
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, midi.messages[0].type);
    TEST_ASSERT_EQUAL(60, midi.messages[0].d1);
    TEST_ASSERT_EQUAL(1, midi.messages[0].channel);
}

// ---------------------------------------------------------------------------
// The worked example from #9, with a gate jack standing in for ClockDiv (#4):
// a chord held on note bus 0, one trigger on gate bus 0, one note on each
// MIDI output an octave apart.
// ---------------------------------------------------------------------------
static void test_worked_example_arpeggio_to_two_outputs_an_octave_apart() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_SERIAL_1, 0, 0};                   // DIN 1 -> note bus 0
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};                    // trigger -> gate bus 0
    p.nodes[0] = node_config(ALGO_ARPEGGIATOR);                           // note 0 (held), gate 0 (advance) -> note 1
    p.nodes[0].in_bus[0] = 0; p.nodes[0].in_bus[1] = 0; p.nodes[0].out_bus[0] = 1;
    p.nodes[1] = node(ALGO_TRANSPOSE, 1, 2);                              // note 1 -> note 2, +12
    p.nodes[1].params[0] = 12;
    p.n_nodes = 2;
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 1, 1};                    // note 1 -> USB 0, channel 1
    p.midi_out[1] = MidiOutConfig{mmMIDI_SERIAL_2, 2, 2};                 // note 2 -> DIN 2, channel 2
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    TEST_ASSERT_EQUAL(1, master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 60, 100}));
    TEST_ASSERT_EQUAL(1, master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 64, 100}));
    TEST_ASSERT_EQUAL(1, master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 67, 100}));
    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_USB_1, MidiEvent{MIDI_NOTE_ON, 1, 72, 100}));   // not routed
    run_passes(master, 3, now);
    TEST_ASSERT_EQUAL(0, midi.messages.size());

    gpio.set_input(0, GPIO_HIGH);           // one trigger
    run_passes(master, 5, now);
    gpio.set_input(0, GPIO_LOW);
    run_passes(master, 5, now);

    TEST_ASSERT_EQUAL(2, midi.messages.size());
    TEST_ASSERT_EQUAL(mmMIDI_USB_0, midi.messages[0].target);
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, midi.messages[0].type);
    TEST_ASSERT_EQUAL(60, midi.messages[0].d1);
    TEST_ASSERT_EQUAL(1, midi.messages[0].channel);
    TEST_ASSERT_EQUAL(mmMIDI_SERIAL_2, midi.messages[1].target);
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, midi.messages[1].type);
    TEST_ASSERT_EQUAL(72, midi.messages[1].d1);
    TEST_ASSERT_EQUAL(2, midi.messages[1].channel);
}

// Fan-out: one bus with three readers delivers to all three in a single pass.
static void test_fan_out_three_readers_one_pass() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_USB_0, 0, 3};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_1, 0, 3};
    p.midi_out[1] = MidiOutConfig{mmMIDI_SERIAL_1, 0, 3};
    p.midi_out[2] = MidiOutConfig{mmMIDI_HOST_1, 0, 3};
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 4};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 4};
    p.gate_ports[2] = GatePortConfig{GATE_PORT_OUT, 4};
    p.gate_ports[3] = GatePortConfig{GATE_PORT_OUT, 4};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_ON, 1, 60, 100});
    gpio.set_input(0, GPIO_HIGH);
    master.pass(0);
    TEST_ASSERT_EQUAL(3, midi.messages.size());
    TEST_ASSERT_EQUAL(mmMIDI_USB_1, midi.messages[0].target);
    TEST_ASSERT_EQUAL(mmMIDI_SERIAL_1, midi.messages[1].target);
    TEST_ASSERT_EQUAL(mmMIDI_HOST_1, midi.messages[2].target);
    for (uint8_t port = 1; port <= 3; port++) TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[port]);
}

// Fan-in: two gate writers OR; two note writers interleave in arrival order;
// overflow is counted.
static void test_fan_in_rules() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_IN, 0};
    p.gate_ports[2] = GatePortConfig{GATE_PORT_OUT, 0};
    p.midi_in[0] = MidiInConfig{mmMIDI_USB_0, 0, 0};
    p.midi_in[1] = MidiInConfig{mmMIDI_SERIAL_1, 0, 0};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_1, 0, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    const uint8_t expected_or[4] = {0, 1, 1, 1};
    for (uint8_t combo = 0; combo < 4; combo++) {
        gpio.set_input(0, combo & 1);
        gpio.set_input(1, (combo >> 1) & 1);
        master.pass(0);
        TEST_ASSERT_EQUAL(expected_or[combo], gpio.outputs[2]);
    }

    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_ON, 1, 1, 100});
    master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 2, 100});
    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_ON, 1, 3, 100});
    master.pass(0);
    TEST_ASSERT_EQUAL(3, midi.messages.size());
    TEST_ASSERT_EQUAL(1, midi.messages[0].d1);
    TEST_ASSERT_EQUAL(2, midi.messages[1].d1);
    TEST_ASSERT_EQUAL(3, midi.messages[2].d1);

    midi.clear();
    for (uint8_t i = 0; i < NOTE_QUEUE_DEPTH + 5; i++) master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_ON, 1, i, 100});
    master.pass(0);
    TEST_ASSERT_EQUAL(NOTE_QUEUE_DEPTH, midi.messages.size());
    TEST_ASSERT_EQUAL(5, master.buses().note_overflows(0));
}

// A patch assigning a gate bus index to a note inlet is rejected by the
// validator, and the running patch is left untouched.
static void test_validator_rejects_wrong_domain_index_and_keeps_running_patch() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch good = empty_patch();
    good.nodes[0] = node(ALGO_LOGIC_NOT, 0, 1);
    good.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(good));
    TEST_ASSERT_EQUAL(1, master.node_count());

    Patch bad = empty_patch();
    bad.nodes[0] = node(ALGO_LOGIC_NOT, 0, 1);
    bad.nodes[1] = node(ALGO_TRANSPOSE, 12, 0);           // 12 is a gate bus index; only 8 note buses exist
    bad.n_nodes = 2;
    TEST_ASSERT_EQUAL(LOAD_NODE_INVALID, master.load(bad));
    TEST_ASSERT_EQUAL(CONFIG_INLET_OUT_OF_RANGE, master.last_node_error());
    TEST_ASSERT_EQUAL(1, master.last_node_index());
    TEST_ASSERT_EQUAL(1, master.node_count());           // still the good patch

    Patch missing = empty_patch();
    missing.nodes[0] = node_config(ALGO_ARPEGGIATOR);
    missing.nodes[0].in_bus[0] = 0; missing.nodes[0].out_bus[0] = 1;   // advance inlet left NO_BUS
    missing.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_NODE_INVALID, master.load(missing));
    TEST_ASSERT_EQUAL(CONFIG_INLET_NOT_CONNECTED, master.last_node_error());

    Patch unknown = empty_patch();
    unknown.nodes[0] = node(99, 0, 1);
    unknown.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_NODE_INVALID, master.load(unknown));
    TEST_ASSERT_EQUAL(CONFIG_UNKNOWN_ALGORITHM, master.last_node_error());

    Patch port = empty_patch();
    port.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, N_GATE_BUS};
    TEST_ASSERT_EQUAL(LOAD_GATE_PORT_BUS_OUT_OF_RANGE, master.load(port));
    Patch midi_port = empty_patch();
    midi_port.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, N_NOTE_BUS};
    TEST_ASSERT_EQUAL(LOAD_MIDI_PORT_BUS_OUT_OF_RANGE, master.load(midi_port));
    Patch too_many = empty_patch();
    too_many.n_nodes = N_NODE + 1;
    TEST_ASSERT_EQUAL(LOAD_TOO_MANY_NODES, master.load(too_many));
}

// A node whose output bus is its own input oscillates at half the pass rate
// instead of hanging or recursing.
static void test_self_feedback_oscillates() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node(ALGO_LOGIC_NOT, 0, 0);
    p.n_nodes = 1;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    uint8_t previous = 0xFF;
    for (int i = 0; i < 20; i++) {
        master.pass(0);
        TEST_ASSERT_NOT_EQUAL(previous, gpio.outputs[0]);
        previous = gpio.outputs[0];
    }
}

// Latency through an n-stage gate chain is exactly n passes.
static void test_chain_latency_is_exactly_n_passes() {
    for (uint8_t stages = 0; stages <= 6; stages += 2) {
        FakeGpio gpio; RecordingMidiOut midi;
        MixedModeMaster master(gpio, midi);
        Patch p = empty_patch();
        p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
        for (uint8_t s = 0; s < stages; s++) p.nodes[s] = node(ALGO_LOGIC_NOT, s, s + 1);
        p.n_nodes = stages;
        p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, stages};   // an even number of inverters: same polarity
        TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
        master.setup();
        for (int i = 0; i < 20; i++) master.pass(0);
        TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[1]);

        gpio.set_input(0, GPIO_HIGH);
        int passes = 0;
        while (gpio.outputs[1] != GPIO_HIGH && passes < 50) { master.pass(0); passes++; }
        // Sampled in pass 1, visible at the jack after `stages` further passes.
        TEST_ASSERT_EQUAL_MESSAGE(stages + 1, passes, "latency in passes (including the sampling pass)");
    }
}

// Eight instances of the same algorithm, and a patch mixing every algorithm
// type, load and run.
static void test_eight_of_the_same_and_one_of_everything() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    for (uint8_t i = 0; i < 8; i++) {
        p.gate_ports[i] = GatePortConfig{(uint8_t)(i < 4 ? GATE_PORT_IN : GATE_PORT_OUT), (uint8_t)(i < 4 ? i : i + 4)};
        p.nodes[i] = node(ALGO_LOGIC_NOT, i, i + 8);
    }
    p.n_nodes = 8;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    gpio.set_input(1, GPIO_HIGH);
    for (int i = 0; i < 4; i++) master.pass(0);
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[4]);
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio.outputs[5]);
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[6]);
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio.outputs[7]);

    Patch all = empty_patch();
    all.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    all.midi_in[0] = MidiInConfig{mmMIDI_USB_0, 0, 0};
    uint8_t n = 0;
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        NodeConfig c = node_config(d->id);
        for (uint8_t k = 0; k < d->min_in; k++) c.in_bus[k] = 0;
        for (uint8_t k = 0; k < d->n_out; k++) c.out_bus[k] = 1;
        all.nodes[n++] = c;
    }
    all.n_nodes = n;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(all));
    master.setup();
    TEST_ASSERT_EQUAL(registry::count(), master.node_count());
    // With the clock running, so the nodes that subscribe to the tick get one.
    for (int i = 0; i < 100; i++) {
        master.clock().advance();
        master.deliver_midi(mmMIDI_USB_0, MidiEvent{(uint8_t)((i & 1) ? MIDI_NOTE_OFF : MIDI_NOTE_ON), 1, 60, 100});
        gpio.set_input(0, (i & 2) ? GPIO_HIGH : GPIO_LOW);
        master.pass((uint32_t)i * 1000);
    }
}

// Zero heap allocation after setup(), asserted by instrumenting operator new.
static void test_zero_heap_allocation_after_setup() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.midi_in[0] = MidiInConfig{mmMIDI_USB_0, 0, 0};
    p.nodes[0] = node_config(ALGO_ARPEGGIATOR);
    p.nodes[0].in_bus[0] = 0; p.nodes[0].in_bus[1] = 0; p.nodes[0].out_bus[0] = 1;
    p.nodes[1] = node(ALGO_TRANSPOSE, 1, 2);
    p.nodes[2] = node(ALGO_SUSTAIN, 0, 2);
    // The largest nodes in the system (#13, #14), advanced by the jack.
    p.nodes[3] = node(ALGO_POLY_SEQ, 0, 3);
    p.nodes[3].params[16 + 1] = 100;                                      // step 0, voice 0 sounds
    p.nodes[4] = node(ALGO_DRUM_SEQ_MIDI, 0, 3);
    p.nodes[4].params[80] = 100;                                          // lane 0, step 0
    p.nodes[5] = node(ALGO_DRUM_SEQ_GATE, 0, 4);
    p.nodes[5].params[16] = 1;                                            // lane 0, step 0
    p.nodes[6] = node(ALGO_NOTE_SEQ, 0, 3);
    p.n_nodes = 7;
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_1, 0, 2};
    p.midi_out[1] = MidiOutConfig{mmMIDI_USB_2, 0, 3};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 0};
    p.gate_ports[2] = GatePortConfig{GATE_PORT_OUT, 4};

    const size_t before = g_allocations;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    uint32_t now = 0;
    for (int i = 0; i < 1000; i++) {
        master.deliver_midi(mmMIDI_USB_0, MidiEvent{(uint8_t)((i & 1) ? MIDI_NOTE_OFF : MIDI_NOTE_ON), 1, 60, 100});
        gpio.set_input(0, i & 1);
        run_passes(master, 1, now);
    }
    master.unload();                        // the handover releases notes: no allocation there either
    TEST_ASSERT_EQUAL(before, g_allocations);
}

// Loading and unloading patches 1000 times leaves memory in an identical state.
static void test_thousand_load_unload_cycles_leave_identical_state() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 1};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 0};
    for (uint8_t i = 0; i < N_NODE; i++) p.nodes[i] = node((uint8_t)(ALGO_LOGIC_NOT + (i % 7)), 0, 1);
    p.n_nodes = N_NODE;

    const size_t before = g_allocations;
    for (int cycle = 0; cycle < 1000; cycle++) {
        TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
        master.setup();
        master.pass(0);
        TEST_ASSERT_EQUAL(N_NODE, master.node_count());
        master.unload();
        TEST_ASSERT_EQUAL(0, master.node_count());
        TEST_ASSERT_EQUAL(GPIO_MODE_INPUT_PULLUP, gpio.modes[1]);   // output returned to input
    }
    TEST_ASSERT_EQUAL(before, g_allocations);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));                   // still loads after 1000 cycles
    TEST_ASSERT_EQUAL(N_NODE, master.node_count());
}

// ---------------------------------------------------------------------------
// The worked example from #9, now with the real ClockDiv from #4 in place of
// the gate jack that stood in for it: the master tick drives a divider, the
// divider advances an arpeggiator, and two MIDI outputs play an octave apart.
// ---------------------------------------------------------------------------
static void test_worked_example_with_a_real_clock_divider() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_SERIAL_1, 0, 0};        // DIN 1 -> note bus 0
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);                  // tick -> gate bus 0
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 4;                                  // /4
    p.nodes[1] = node_config(ALGO_ARPEGGIATOR);                // note 0 + gate 0 -> note 1
    p.nodes[1].in_bus[0] = 0; p.nodes[1].in_bus[1] = 0; p.nodes[1].out_bus[0] = 1;
    p.nodes[2] = node(ALGO_TRANSPOSE, 1, 2);                   // note 1 -> note 2, +12
    p.nodes[2].params[0] = 12;
    p.n_nodes = 3;
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 1, 1};
    p.midi_out[1] = MidiOutConfig{mmMIDI_SERIAL_2, 2, 2};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 60, 100});
    master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 64, 100});
    master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_NOTE_ON, 1, 67, 100});

    // Eight beats of the divider, one subtick per pass at 1 ms.
    uint32_t now = 0;
    for (uint32_t t = 0; t < 8 * 4 * CLOCK_SUBTICK; t++) {
        master.clock().advance();
        master.pass(now);
        now += 1000;
    }
    run_passes(master, 4, now);          // let the last edge through the chain

    // Every note played on USB 0 has its octave twin on DIN 2, one channel
    // each, and the arpeggio ran up the chord.
    uint8_t usb_notes = 0, din_notes = 0;
    uint8_t expected[3] = {60, 64, 67};
    for (const RecordingMidiOut::Message& m : midi.messages) {
        if (m.type != MIDI_NOTE_ON) continue;
        if (m.target == mmMIDI_USB_0) {
            TEST_ASSERT_EQUAL(1, m.channel);
            TEST_ASSERT_EQUAL(expected[usb_notes % 3], m.d1);
            usb_notes++;
        } else if (m.target == mmMIDI_SERIAL_2) {
            TEST_ASSERT_EQUAL(2, m.channel);
            TEST_ASSERT_EQUAL(expected[din_notes % 3] + 12, m.d1);
            din_notes++;
        }
    }
    TEST_ASSERT_EQUAL(8, usb_notes);
    TEST_ASSERT_EQUAL(8, din_notes);
}

// Chaining is the point: every modifier's output is legal input to every
// other. Transpose -> arpeggiator -> note priority over three buses, with the
// arpeggiator advanced by a divider.
static void test_transpose_arpeggiator_priority_chain() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_USB_0, 0, 0};
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 2;
    p.nodes[1] = node(ALGO_TRANSPOSE, 0, 1);                   // note 0 -> note 1, -12
    p.nodes[1].params[0] = (uint8_t)(int8_t)-12;
    p.nodes[2] = node_config(ALGO_ARPEGGIATOR);                // note 1 + gate 0 -> note 2
    p.nodes[2].in_bus[0] = 1; p.nodes[2].in_bus[1] = 0; p.nodes[2].out_bus[0] = 2;
    p.nodes[3] = node(ALGO_NOTE_PRIORITY, 2, 3);               // note 2 -> note 3
    p.n_nodes = 4;
    p.midi_out[0] = MidiOutConfig{mmMIDI_SERIAL_1, 0, 3};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_ON, 1, 72, 100});
    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_ON, 1, 76, 100});

    uint32_t now = 0;
    for (uint32_t t = 0; t < 6 * 2 * CLOCK_SUBTICK; t++) {
        master.clock().advance();
        master.pass(now);
        now += 1000;
    }
    run_passes(master, 4, now);          // let the last edge through the chain

    // The chord arrived an octave down, and alternates through the arpeggio.
    uint8_t ons = 0;
    int8_t sounding = 0;
    const uint8_t expected[2] = {60, 64};
    for (const RecordingMidiOut::Message& m : midi.messages) {
        TEST_ASSERT_EQUAL(mmMIDI_SERIAL_1, m.target);
        if (m.type == MIDI_NOTE_ON) {
            TEST_ASSERT_EQUAL(expected[ons % 2], m.d1);
            ons++;
            sounding++;
        } else {
            sounding--;
        }
        TEST_ASSERT_TRUE(sounding >= 0 && sounding <= 1);       // monophonic, always
    }
    TEST_ASSERT_EQUAL(6, ons);

    // Lifting the chord leaves nothing sounding anywhere in the chain.
    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_OFF, 1, 72, 0});
    master.deliver_midi(mmMIDI_USB_0, MidiEvent{MIDI_NOTE_OFF, 1, 76, 0});
    run_passes(master, 10, now);
    int8_t final_sounding = 0;
    for (const RecordingMidiOut::Message& m : midi.messages) {
        final_sounding = (int8_t)(final_sounding + (m.type == MIDI_NOTE_ON ? 1 : -1));
    }
    TEST_ASSERT_EQUAL(0, final_sounding);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_gate_in_through_algorithms_to_midi_out);
    RUN_TEST(test_worked_example_arpeggio_to_two_outputs_an_octave_apart);
    RUN_TEST(test_fan_out_three_readers_one_pass);
    RUN_TEST(test_fan_in_rules);
    RUN_TEST(test_validator_rejects_wrong_domain_index_and_keeps_running_patch);
    RUN_TEST(test_self_feedback_oscillates);
    RUN_TEST(test_chain_latency_is_exactly_n_passes);
    RUN_TEST(test_eight_of_the_same_and_one_of_everything);
    RUN_TEST(test_worked_example_with_a_real_clock_divider);
    RUN_TEST(test_transpose_arpeggiator_priority_chain);
    RUN_TEST(test_zero_heap_allocation_after_setup);
    RUN_TEST(test_thousand_load_unload_cycles_leave_identical_state);
    return UNITY_END();
}
