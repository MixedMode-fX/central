#include <unity.h>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "clock/master_clock.h"
#include "algorithm/clock/clock_div.h"
#include "master.h"
#include "hal/midi_types.h"

void setUp() {}
void tearDown() {}

// A fake tick source: the test calls advance() where the interval timer's ISR
// would, so every clock and divider assertion below runs on the host.
static void advance(MasterClock& clock, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) clock.advance();
}

// ---------------------------------------------------------------------------
// MasterClock
// ---------------------------------------------------------------------------

static void test_internal_interval_follows_tempo() {
    MasterClock clock;
    clock.set_source(MasterClock::CLOCK_INTERNAL);
    clock.set_bpm(120);
    // 60 s / (120 BPM * 576 subticks per quarter) = 868.05 us
    TEST_ASSERT_EQUAL_UINT32(868, clock.subtick_interval_us());
    clock.set_bpm(60);
    TEST_ASSERT_EQUAL_UINT32(1736, clock.subtick_interval_us());
    // Out of range tempos are clamped, never accepted and mis-scheduled.
    clock.set_bpm(1);
    TEST_ASSERT_EQUAL_UINT16(CLOCK_MIN_BPM, clock.bpm());
    clock.set_bpm(60000);
    TEST_ASSERT_EQUAL_UINT16(CLOCK_MAX_BPM, clock.bpm());
}

// The main loop reprograms the timer, so the flag is what tells it to.
static void test_interval_change_is_reported_once() {
    MasterClock clock;
    TEST_ASSERT_TRUE(clock.take_interval_change());   // the boot interval
    TEST_ASSERT_FALSE(clock.take_interval_change());
    clock.set_bpm(140);
    TEST_ASSERT_TRUE(clock.take_interval_change());
    TEST_ASSERT_FALSE(clock.take_interval_change());
    clock.set_bpm(140);                               // same tempo, no change
    TEST_ASSERT_FALSE(clock.take_interval_change());
}

static void test_count_only_moves_while_running() {
    MasterClock clock;
    advance(clock, 10);
    TEST_ASSERT_EQUAL_UINT32(10, clock.count());
    clock.stop();
    advance(clock, 10);
    TEST_ASSERT_EQUAL_UINT32(10, clock.count());
    clock.resume();
    advance(clock, 5);
    TEST_ASSERT_EQUAL_UINT32(15, clock.count());
    clock.start();                                    // start is the downbeat
    TEST_ASSERT_EQUAL_UINT32(0, clock.count());
}

// Subticks that arrive between two passes are collapsed into one report.
static void test_consume_collapses_and_reports_once() {
    MasterClock clock;
    uint32_t count = 0xFFFFFFFF;
    TEST_ASSERT_FALSE(clock.consume(count));
    advance(clock, 3);
    TEST_ASSERT_TRUE(clock.consume(count));
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_FALSE(clock.consume(count));
}

// MIDI clock bytes drive the count, and the measured period sets the interval
// the timer fills the gaps with.
static void test_midi_clock_advances_and_measures() {
    MasterClock clock;
    clock.set_source(MasterClock::CLOCK_MIDI);
    clock.start();
    uint32_t now = 1000;
    const uint32_t period = 20833;                    // 120 BPM at 24 PPQN
    for (int i = 0; i < 4; i++) {
        clock.midi_message(MIDI_CLOCK, now);
        now += period;
    }
    TEST_ASSERT_EQUAL_UINT32(4 * CLOCK_SUBTICK, clock.count());
    TEST_ASSERT_EQUAL_UINT32(period / CLOCK_SUBTICK, clock.subtick_interval_us());
    TEST_ASSERT_EQUAL_UINT32(0, clock.rejected_edges());
}

// Between edges the timer keeps the count moving; the next edge re-phases it
// onto the boundary rather than letting the estimate accumulate error.
static void test_external_edges_rephase_without_rewinding() {
    MasterClock clock;
    clock.set_source(MasterClock::CLOCK_MIDI);
    clock.start();
    clock.midi_message(MIDI_CLOCK, 1000);             // edge 1 -> subtick 24
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICK, clock.count());
    advance(clock, 5);                                // timer ran slow
    clock.midi_message(MIDI_CLOCK, 21000);            // edge 2 -> subtick 48
    TEST_ASSERT_EQUAL_UINT32(2 * CLOCK_SUBTICK, clock.count());

    advance(clock, CLOCK_SUBTICK + 4);                // timer ran fast
    const uint32_t ahead = clock.count();
    clock.midi_message(MIDI_CLOCK, 41000);
    TEST_ASSERT_EQUAL_UINT32(ahead, clock.count());   // never rewinds
}

static void test_implausible_edge_periods_are_counted_not_used() {
    MasterClock clock;
    clock.set_source(MasterClock::CLOCK_MIDI);
    clock.start();
    clock.midi_message(MIDI_CLOCK, 1000);
    const uint32_t before = clock.subtick_interval_us();
    clock.midi_message(MIDI_CLOCK, 1010);             // 10 us apart: noise
    TEST_ASSERT_EQUAL_UINT32(before, clock.subtick_interval_us());
    TEST_ASSERT_EQUAL_UINT32(1, clock.rejected_edges());
}

static void test_midi_transport_messages() {
    MasterClock clock;
    clock.set_source(MasterClock::CLOCK_MIDI);
    advance(clock, 7);
    clock.midi_message(MIDI_STOP, 0);
    TEST_ASSERT_FALSE(clock.running());
    advance(clock, 7);
    TEST_ASSERT_EQUAL_UINT32(7, clock.count());
    clock.midi_message(MIDI_CONTINUE, 0);             // resumes where it was
    TEST_ASSERT_TRUE(clock.running());
    TEST_ASSERT_EQUAL_UINT32(7, clock.count());
    clock.midi_message(MIDI_START, 0);                // start is from zero
    TEST_ASSERT_EQUAL_UINT32(0, clock.count());
}

// A sync pulse stands for a quarter divided by cv_ppqn, so the same edge
// arrives at a different count depending on what the jack is patched from.
static void test_cv_sync_pulses_carry_more_than_one_tick() {
    MasterClock clock;
    clock.set_source(MasterClock::CLOCK_CV);
    clock.set_cv_ppqn(4);                             // 16ths
    clock.start();
    clock.external_edge(1000);
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 4, clock.count());
    clock.external_edge(126000);
    TEST_ASSERT_EQUAL_UINT32(2 * (CLOCK_SUBTICKS_PER_QUARTER / 4), clock.count());
}

// ---------------------------------------------------------------------------
// ClockDiv, driven directly through a BusManager
// ---------------------------------------------------------------------------

static NodeConfig div_config(uint8_t mode, uint8_t amount, uint8_t out_bus) {
    NodeConfig c = node_config(ALGO_CLOCK_DIV);
    c.out_bus[0] = out_bus;
    c.params[0] = mode;
    c.params[1] = amount;
    return c;
}

// Runs `subticks` subticks through a node and returns the counts at which its
// output bus went high. One subtick per pass at 1 ms, which is 1000 BPM at
// this subdivision - fast enough that a 5 ms trigger spans several passes and
// slow enough that consecutive pulses still separate.
struct DivRun {
    uint32_t pulses;
    uint32_t first_high;
    bool ever_high;
};

static DivRun run_ticks(ClockDiv& node, BusManager& bus, uint8_t out_bus,
                        uint32_t subticks, uint32_t start_count = 0,
                        uint32_t step_us = 1000) {
    DivRun r = {0, 0, false};
    bool was_high = false;
    uint32_t now = 0;
    for (uint32_t t = 0; t < subticks; t++) {
        node.process(bus, now);
        node.tick(bus, start_count + t);
        bus.swap();
        const bool high = bus.gate_read(out_bus);
        if (high && !was_high) {
            r.pulses++;
            if (!r.ever_high) { r.first_high = start_count + t; r.ever_high = true; }
        }
        was_high = high;
        now += step_us;
    }
    return r;
}

// Divide by four: one pulse every four PPQN ticks, on the tick.
static void test_divide_by_four_fires_every_four_ticks() {
    BusManager bus;
    NodeConfig c = div_config(0, 4, 2);
    ClockDiv node(c);
    TEST_ASSERT_EQUAL_UINT32(4 * CLOCK_SUBTICK, node.period());
    const DivRun r = run_ticks(node, bus, 2, 40 * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(10, r.pulses);
    TEST_ASSERT_TRUE(r.ever_high);
    TEST_ASSERT_EQUAL_UINT32(0, r.first_high);
}

// Multiply by four: four pulses per PPQN tick, exactly.
static void test_multiply_by_four_fires_four_times_per_tick() {
    BusManager bus;
    NodeConfig c = div_config(1, 4, 3);
    c.params[4] = 1;                                   // 1 ms pulses, so they separate
    ClockDiv node(c);
    TEST_ASSERT_FALSE(node.multiply_refused());
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICK / 4, node.period());
    const DivRun r = run_ticks(node, bus, 3, 10 * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(40, r.pulses);
}

// A multiplier that does not divide the subdivision is refused, loudly, and
// runs at x1 rather than placing pulses on fractional subticks.
static void test_inexact_multiplier_is_refused() {
    BusManager bus;
    NodeConfig c = div_config(1, 5, 4);
    ClockDiv node(c);
    TEST_ASSERT_TRUE(node.multiply_refused());
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICK, node.period());
    const DivRun r = run_ticks(node, bus, 4, 10 * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(10, r.pulses);
}

// phase rotates the pattern inside its own cycle; delay lags it by whole ticks.
static void test_phase_and_delay_shift_the_output() {
    BusManager bus;
    NodeConfig c = div_config(0, 4, 5);
    c.params[2] = 128;                                 // half an output step
    ClockDiv node(c);
    DivRun r = run_ticks(node, bus, 5, 20 * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(2 * CLOCK_SUBTICK, r.first_high);
    TEST_ASSERT_EQUAL_UINT32(5, r.pulses);

    BusManager bus2;
    NodeConfig d = div_config(0, 4, 5);
    d.params[3] = 3;                                   // three whole ticks
    ClockDiv delayed(d);
    r = run_ticks(delayed, bus2, 5, 20 * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(3 * CLOCK_SUBTICK, r.first_high);
}

// Trigger width is wall-clock and does not follow tempo: the same node, run
// at two tempi, holds its output up for the same number of microseconds.
static void test_pulse_width_does_not_follow_tempo() {
    for (int slow = 0; slow < 2; slow++) {
        BusManager bus;
        NodeConfig c = div_config(0, 1, 6);
        ClockDiv node(c);
        const uint32_t step_us = slow ? 2000 : 200;    // 10x tempo difference
        uint32_t now = 0;
        uint32_t high_us = 0;
        for (uint32_t t = 0; t < 4 * CLOCK_SUBTICK; t++) {
            node.process(bus, now);
            node.tick(bus, t);
            bus.swap();
            if (bus.gate_read(6)) high_us += step_us;
            now += step_us;
        }
        // Four pulses of TRIGGER_WIDTH_US, sampled at step_us: the sampled
        // total is within one sample per pulse of the real width.
        const uint32_t expected = 4 * TRIGGER_WIDTH_US;
        TEST_ASSERT_UINT32_WITHIN(4 * step_us, expected, high_us);
    }
}

// Two dividers on one tick keep a fixed relationship for as long as they run.
static void test_two_dividers_do_not_drift() {
    BusManager bus;
    NodeConfig a = div_config(0, 3, 7);
    NodeConfig b = div_config(0, 4, 8);
    ClockDiv three(a);
    ClockDiv four(b);
    uint32_t now = 0;
    uint32_t coincidences = 0;
    bool was_a = false, was_b = false;
    for (uint32_t t = 0; t < 10000u * CLOCK_SUBTICK; t++) {
        three.process(bus, now); four.process(bus, now);
        three.tick(bus, t);      four.tick(bus, t);
        bus.swap();
        const bool ha = bus.gate_read(7), hb = bus.gate_read(8);
        if (ha && !was_a && hb && !was_b) {
            // Both fire together only on multiples of 12 ticks, for ever.
            TEST_ASSERT_EQUAL_UINT32(0, t % (12u * CLOCK_SUBTICK));
            coincidences++;
        }
        was_a = ha; was_b = hb;
        now += 100;
    }
    // 10,000 ticks hold 834 multiples of 12, 3334 of 3 and 2500 of 4, counting
    // the downbeat. Every one of them landed where it should have.
    TEST_ASSERT_EQUAL_UINT32(834, coincidences);
    TEST_ASSERT_EQUAL_UINT32(3334, three.pulses());
    TEST_ASSERT_EQUAL_UINT32(2500, four.pulses());
}

// A node loaded into a patch that has been running for a long time starts on
// the pattern, not on the next subtick.
static void test_late_start_lands_on_the_pattern() {
    BusManager bus;
    NodeConfig c = div_config(0, 4, 9);
    ClockDiv node(c);
    const uint32_t start = 1000u * CLOCK_SUBTICK + 7u;
    const DivRun r = run_ticks(node, bus, 9, 8 * CLOCK_SUBTICK, start);
    TEST_ASSERT_TRUE(r.ever_high);
    TEST_ASSERT_EQUAL_UINT32(0, r.first_high % (4u * CLOCK_SUBTICK));
}

// The clock restarting (a MIDI start) puts every divider back on the downbeat.
static void test_clock_restart_resets_the_pattern() {
    BusManager bus;
    NodeConfig c = div_config(0, 4, 10);
    ClockDiv node(c);
    run_ticks(node, bus, 10, 10 * CLOCK_SUBTICK);
    const uint32_t before = node.pulses();
    const DivRun r = run_ticks(node, bus, 10, 4 * CLOCK_SUBTICK, 0);   // count went back to 0
    TEST_ASSERT_EQUAL_UINT32(before + 1, node.pulses());
    TEST_ASSERT_EQUAL_UINT32(0, r.first_high);
}

// ---------------------------------------------------------------------------
// Through MixedModeMaster: the clock, the pool and the jacks together
// ---------------------------------------------------------------------------

// Rising edges on a jack, counted over a run of passes.
struct EdgeCount {
    uint32_t rises;
    uint32_t last_rise_pass;
};

static EdgeCount run_master(MixedModeMaster& master, FakeGpio& gpio, uint8_t port,
                            uint32_t passes, uint32_t subticks_per_pass,
                            uint32_t step_us = 1000) {
    EdgeCount e = {0, 0};
    bool was = false;
    uint32_t now = 0;
    for (uint32_t i = 0; i < passes; i++) {
        for (uint32_t s = 0; s < subticks_per_pass; s++) master.clock().advance();
        master.pass(now);
        const bool high = gpio.outputs[port] == GPIO_HIGH;
        if (high && !was) { e.rises++; e.last_rise_pass = i; }
        was = high;
        now += step_us;
    }
    return e;
}

// ClockDiv -> jack, driven by the clock the master owns: no test-side tick
// injection, the fake tick source is the only thing standing in for hardware.
static void test_clock_div_drives_a_jack_through_the_master() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);              // tick source -> gate 0
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 4;                              // /4
    p.n_nodes = 1;
    p.gate_ports[3] = GatePortConfig{GATE_PORT_OUT, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    const EdgeCount e = run_master(master, gpio, 3, 10 * 4 * CLOCK_SUBTICK, 1);
    TEST_ASSERT_EQUAL_UINT32(10, e.rises);
}

// A divider dividing another divider: the chain resolves at gate rate, one
// pass per stage, not one tick.
static void test_divider_chained_from_another_divider() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);              // tick -> gate 0, /4
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 4;
    p.nodes[1] = node_config(ALGO_CLOCK_DIV);              // gate 0 -> gate 1, /3
    p.nodes[1].in_bus[0] = 0;
    p.nodes[1].out_bus[0] = 1;
    p.nodes[1].params[1] = 3;
    p.n_nodes = 2;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 0};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 1};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    const uint32_t ticks = 36 * 4 * CLOCK_SUBTICK;         // 36 pulses from /4
    EdgeCount first = {0, 0};
    EdgeCount second = {0, 0};
    bool wa = false, wb = false;
    uint32_t now = 0;
    for (uint32_t t = 0; t < ticks; t++) {
        master.clock().advance();
        master.pass(now);
        const bool a = gpio.outputs[0] == GPIO_HIGH;
        const bool b = gpio.outputs[1] == GPIO_HIGH;
        if (a && !wa) { first.rises++; first.last_rise_pass = t; }
        if (b && !wb) {
            second.rises++;
            second.last_rise_pass = t;
            // The second stage answers the first within a couple of passes -
            // microseconds - and never a tick-to-tick 20 ms later.
            TEST_ASSERT_UINT32_WITHIN(3, first.last_rise_pass, t);
        }
        wa = a; wb = b;
        now += 200;
    }
    TEST_ASSERT_EQUAL_UINT32(36, first.rises);
    TEST_ASSERT_EQUAL_UINT32(12, second.rises);
}

// Multiplication from a gate source is refused rather than approximated: the
// node runs at x1 and passes the edges through.
static void test_multiply_from_a_gate_source_is_refused() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_CLOCK_DIV);
    c.in_bus[0] = 0;
    c.out_bus[0] = 1;
    c.params[0] = 1;                                       // multiply
    c.params[1] = 4;
    ClockDiv node(c);
    TEST_ASSERT_TRUE(node.multiply_refused());

    uint32_t now = 0;
    uint32_t rises = 0;
    bool was = false;
    for (uint32_t i = 0; i < 40; i++) {
        bus.gate_write(0, (i % 10) < 2);                   // an edge every 10 passes
        bus.swap();
        node.process(bus, now);
        bus.swap();
        const bool high = bus.gate_read(1);
        if (high && !was) rises++;
        was = high;
        now += 1000;
    }
    TEST_ASSERT_EQUAL_UINT32(4, rises);                    // one per input edge
}

// MIDI clock arriving on a transport reaches the clock and drives a divider,
// and note traffic on the same transport still reaches its bus.
static void test_midi_clock_through_deliver_midi_drives_a_divider() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 1;                              // one pulse per tick
    p.n_nodes = 1;
    p.gate_ports[2] = GatePortConfig{GATE_PORT_OUT, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    master.clock().set_source(MasterClock::CLOCK_MIDI);
    master.clock().start();

    uint32_t now = 0;
    uint32_t rises = 0;
    bool was = false;
    for (uint32_t i = 0; i < 8; i++) {
        // A realtime message is transport-level: no MidiInPort accepts it.
        TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_SERIAL_1,
                                                 MidiEvent{MIDI_CLOCK, 0, 0, 0}, now));
        for (int pass = 0; pass < 4; pass++) {
            master.pass(now);
            const bool high = gpio.outputs[2] == GPIO_HIGH;
            if (high && !was) rises++;
            was = high;
            now += 4000;      // 16 ms per MIDI tick, so 5 ms triggers separate
        }
    }
    TEST_ASSERT_EQUAL_UINT32(8, rises);
}

// The sync jack only drives the clock when the CV source is selected.
static void test_sync_edge_only_counts_for_the_cv_source() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    master.clock().set_source(MasterClock::CLOCK_INTERNAL);
    const uint32_t before = master.clock().count();
    master.sync_edge(1000);
    TEST_ASSERT_EQUAL_UINT32(before, master.clock().count());
    master.clock().set_source(MasterClock::CLOCK_CV);
    master.clock().start();
    master.sync_edge(1000);
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / master.clock().cv_ppqn(),
                             master.clock().count());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_internal_interval_follows_tempo);
    RUN_TEST(test_interval_change_is_reported_once);
    RUN_TEST(test_count_only_moves_while_running);
    RUN_TEST(test_consume_collapses_and_reports_once);
    RUN_TEST(test_midi_clock_advances_and_measures);
    RUN_TEST(test_external_edges_rephase_without_rewinding);
    RUN_TEST(test_implausible_edge_periods_are_counted_not_used);
    RUN_TEST(test_midi_transport_messages);
    RUN_TEST(test_cv_sync_pulses_carry_more_than_one_tick);
    RUN_TEST(test_divide_by_four_fires_every_four_ticks);
    RUN_TEST(test_multiply_by_four_fires_four_times_per_tick);
    RUN_TEST(test_inexact_multiplier_is_refused);
    RUN_TEST(test_phase_and_delay_shift_the_output);
    RUN_TEST(test_pulse_width_does_not_follow_tempo);
    RUN_TEST(test_two_dividers_do_not_drift);
    RUN_TEST(test_late_start_lands_on_the_pattern);
    RUN_TEST(test_clock_restart_resets_the_pattern);
    RUN_TEST(test_clock_div_drives_a_jack_through_the_master);
    RUN_TEST(test_divider_chained_from_another_divider);
    RUN_TEST(test_multiply_from_a_gate_source_is_refused);
    RUN_TEST(test_midi_clock_through_deliver_midi_drives_a_divider);
    RUN_TEST(test_sync_edge_only_counts_for_the_cv_source);
    return UNITY_END();
}
