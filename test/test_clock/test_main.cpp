#include <unity.h>
#include <stdio.h>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "clock/master_clock.h"
#include "algorithm/clock/clock_div.h"
#include "algorithm/clock/metronome.h"
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

// ---------------------------------------------------------------------------
// Metronome: the same clock, said in note values
// ---------------------------------------------------------------------------

static NodeConfig metro_config(uint8_t division, uint8_t feel, uint8_t out_bus) {
    NodeConfig c = node_config(ALGO_METRONOME);
    c.out_bus[0] = out_bus;
    c.params[0] = division;
    c.params[1] = feel;
    return c;
}

// Where the pulses landed, and whether they were evenly spaced. An uneven gap
// is what a rate that is not a whole number of subticks looks like from
// outside: the period rounds one way on one pulse and the other way on the
// next.
struct MetroRun {
    uint32_t pulses;
    uint32_t first_high;
    uint32_t gap;
    bool ever_high;
    bool even_gaps;
};

static MetroRun run_metronome(Metronome& node, BusManager& bus, uint8_t out_bus,
                              uint32_t subticks, uint32_t start_count = 0,
                              uint32_t step_us = 1000) {
    MetroRun r = {0, 0, 0, false, true};
    bool was_high = false;
    uint32_t now = 0, last_at = 0;
    for (uint32_t t = 0; t < subticks; t++) {
        node.process(bus, now);
        node.tick(bus, start_count + t);
        bus.swap();
        const bool high = bus.gate_read(out_bus);
        if (high && !was_high) {
            const uint32_t at = start_count + t;
            r.pulses++;
            if (!r.ever_high) { r.first_high = at; r.ever_high = true; }
            else if (r.gap == 0) r.gap = at - last_at;
            else if (at - last_at != r.gap) r.even_gaps = false;
            last_at = at;
        }
        was_high = high;
        now += step_us;
    }
    return r;
}

// The claim the friendly control rests on: every note value, in every feel,
// is a whole number of subticks. Spelled out against the arithmetic a
// musician would do rather than against the table the node reads, so a table
// entry that drifted from its name would fail here.
static void test_every_note_value_is_a_whole_number_of_subticks() {
    const uint32_t Q = CLOCK_SUBTICKS_PER_QUARTER;
    struct Case { uint8_t division; uint8_t feel; uint32_t period; };
    static const Case CASES[] = {
        {Metronome::DIV_8_BARS,  Metronome::FEEL_STRAIGHT, Q * 32},
        {Metronome::DIV_4_BARS,  Metronome::FEEL_STRAIGHT, Q * 16},
        {Metronome::DIV_2_BARS,  Metronome::FEEL_STRAIGHT, Q * 8},
        {Metronome::DIV_BAR,     Metronome::FEEL_STRAIGHT, Q * 4},
        {Metronome::DIV_HALF,    Metronome::FEEL_STRAIGHT, Q * 2},
        {Metronome::DIV_QUARTER, Metronome::FEEL_STRAIGHT, Q},
        {Metronome::DIV_EIGHTH,  Metronome::FEEL_STRAIGHT, Q / 2},
        {Metronome::DIV_16TH,    Metronome::FEEL_STRAIGHT, Q / 4},
        {Metronome::DIV_32ND,    Metronome::FEEL_STRAIGHT, Q / 8},
        {Metronome::DIV_64TH,    Metronome::FEEL_STRAIGHT, Q / 16},
        // A dot is half as long again; a triplet is three in the space of two.
        {Metronome::DIV_QUARTER, Metronome::FEEL_DOTTED,   Q * 3 / 2},
        {Metronome::DIV_EIGHTH,  Metronome::FEEL_DOTTED,   Q * 3 / 4},
        {Metronome::DIV_64TH,    Metronome::FEEL_DOTTED,   Q * 3 / 32},
        {Metronome::DIV_QUARTER, Metronome::FEEL_TRIPLET,  Q * 2 / 3},
        {Metronome::DIV_EIGHTH,  Metronome::FEEL_TRIPLET,  Q / 3},
        {Metronome::DIV_64TH,    Metronome::FEEL_TRIPLET,  Q / 24},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        NodeConfig c = metro_config(CASES[i].division, CASES[i].feel, 0);
        Metronome node(c);
        char message[64];
        snprintf(message, sizeof message, "division %u feel %u",
                 CASES[i].division, CASES[i].feel);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(CASES[i].period, node.period(), message);
    }

    // And nothing in the list rounds, including the values the table above
    // does not spell out: a dot is exactly three halves of its note value and
    // a triplet exactly two thirds, stated as integer equalities so that a
    // period off by one subtick fails rather than passing by a rounding.
    for (uint8_t d = Metronome::DIV_8_BARS; d <= Metronome::DIVISIONS; d++) {
        NodeConfig s_config = metro_config(d, Metronome::FEEL_STRAIGHT, 0);
        NodeConfig d_config = metro_config(d, Metronome::FEEL_DOTTED, 0);
        NodeConfig t_config = metro_config(d, Metronome::FEEL_TRIPLET, 0);
        Metronome straight(s_config), dotted(d_config), triplet(t_config);
        char message[48];
        snprintf(message, sizeof message, "division %u", d);
        TEST_ASSERT_TRUE_MESSAGE(straight.period() > 0, message);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(straight.period() * 3, dotted.period() * 2, message);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(straight.period() * 2, triplet.period() * 3, message);
    }
}

// The acceptance case: a Metronome at a quarter note and a ClockDiv of
// MASTER_PPQN are the same clock. The friendly control is a *name* for the
// arithmetic, not an approximation of it, so the two fire on the same
// subticks for as long as they run.
static void test_a_quarter_note_is_a_divider_of_ppqn() {
    BusManager bus;
    NodeConfig m = metro_config(Metronome::DIV_QUARTER, Metronome::FEEL_STRAIGHT, 0);
    NodeConfig d = div_config(0, MASTER_PPQN, 1);
    Metronome metro(m);
    ClockDiv divider(d);

    uint32_t now = 0, metro_rises = 0, div_rises = 0;
    bool was_metro = false, was_div = false;
    for (uint32_t t = 0; t < 32 * CLOCK_SUBTICKS_PER_QUARTER; t++) {
        metro.process(bus, now);
        divider.process(bus, now);
        metro.tick(bus, t);
        divider.tick(bus, t);
        bus.swap();
        const bool a = bus.gate_read(0);
        const bool b = bus.gate_read(1);
        // Not "the same number of pulses" but "high together on every pass":
        // two nodes a subtick apart would still count the same.
        TEST_ASSERT_EQUAL_MESSAGE(b, a, "the metronome and the divider disagreed");
        if (a && !was_metro) metro_rises++;
        if (b && !was_div) div_rises++;
        was_metro = a; was_div = b;
        now += 1000;
    }
    TEST_ASSERT_EQUAL_UINT32(32, metro_rises);
    TEST_ASSERT_EQUAL_UINT32(32, div_rises);
}

// Three in the space of two, and a dot that is half as long again - counted
// from the output rather than from the period, and over enough bars that a
// rate rounding by one subtick would show up as an uneven gap.
static void test_triplets_and_dots_land_where_they_are_named() {
    BusManager triplets;
    NodeConfig t8 = metro_config(Metronome::DIV_EIGHTH, Metronome::FEEL_TRIPLET, 2);
    Metronome triplet_node(t8);
    const MetroRun tr = run_metronome(triplet_node, triplets, 2, 8 * CLOCK_SUBTICKS_PER_QUARTER);
    TEST_ASSERT_EQUAL_UINT32(24, tr.pulses);                    // three per beat
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 3, tr.gap);
    TEST_ASSERT_TRUE(tr.even_gaps);
    TEST_ASSERT_EQUAL_UINT32(0, tr.first_high);

    // A dotted eighth is three sixteenths, so eight of them span three beats.
    BusManager dotted;
    NodeConfig d8 = metro_config(Metronome::DIV_EIGHTH, Metronome::FEEL_DOTTED, 3);
    Metronome dotted_node(d8);
    const MetroRun dt = run_metronome(dotted_node, dotted, 3, 8 * CLOCK_SUBTICKS_PER_QUARTER);
    TEST_ASSERT_EQUAL_UINT32(11, dt.pulses);                    // 8 beats / 0.75
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER * 3 / 4, dt.gap);
    TEST_ASSERT_TRUE(dt.even_gaps);
}

// The grid is anchored on subtick 0 - the downbeat start() resets to - so a
// metronome added to a patch that has been running lands on the beat rather
// than on the subtick it was constructed on.
static void test_a_metronome_loaded_late_lands_on_the_beat() {
    BusManager bus;
    NodeConfig c = metro_config(Metronome::DIV_QUARTER, Metronome::FEEL_STRAIGHT, 4);
    Metronome node(c);
    const uint32_t start = 1000 * CLOCK_SUBTICKS_PER_QUARTER + 7;      // mid-beat
    const MetroRun r = run_metronome(node, bus, 4, 4 * CLOCK_SUBTICKS_PER_QUARTER, start);
    TEST_ASSERT_TRUE(r.ever_high);
    TEST_ASSERT_EQUAL_UINT32(0, r.first_high % CLOCK_SUBTICKS_PER_QUARTER);
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER, r.gap);
    TEST_ASSERT_TRUE(r.even_gaps);
}

// A new division applies from the next period: the pulse already scheduled
// lands where it was going, as in ClockDiv, rather than being dragged out
// from under a musician counting on it.
static void test_changing_the_division_keeps_the_rate_exact() {
    BusManager bus;
    NodeConfig c = metro_config(Metronome::DIV_QUARTER, Metronome::FEEL_STRAIGHT, 5);
    Metronome node(c);
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER, node.period());

    // Straight to triplet and back, and a division either side of it.
    TEST_ASSERT_TRUE(node.set_param(1, Metronome::FEEL_TRIPLET));
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER * 2 / 3, node.period());
    TEST_ASSERT_TRUE(node.set_param(0, Metronome::DIV_EIGHTH));
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 3, node.period());
    TEST_ASSERT_TRUE(node.set_param(1, Metronome::FEEL_STRAIGHT));
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 2, node.period());

    // A value outside the list is refused, and the node keeps what it had.
    TEST_ASSERT_FALSE(node.set_param(0, Metronome::DIVISIONS + 1));
    TEST_ASSERT_FALSE(node.set_param(1, Metronome::FEELS + 1));
    TEST_ASSERT_EQUAL(Metronome::DIV_EIGHTH, node.division());
    TEST_ASSERT_EQUAL(Metronome::FEEL_STRAIGHT, node.feel());

    // And it still runs at the rate it now reads back at.
    const MetroRun r = run_metronome(node, bus, 5, 8 * CLOCK_SUBTICKS_PER_QUARTER);
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 2, r.gap);
    TEST_ASSERT_TRUE(r.even_gaps);
}

// The reset inlet is a downbeat: the grid re-anchors on the edge and the next
// division is counted from there. Driven through the master from a jack,
// because "another node can re-phase it" is the whole point of it being an
// inlet rather than a parameter.
static void test_reset_re_anchors_the_grid() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[7] = GatePortConfig{GATE_PORT_IN, 0};     // jack 8 -> gate bus 0
    p.nodes[0] = node_config(ALGO_METRONOME);
    p.nodes[0].in_bus[0] = 0;                              // reset
    p.nodes[0].out_bus[0] = 1;
    p.nodes[0].params[0] = Metronome::DIV_QUARTER;
    p.n_nodes = 1;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 1};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    // Four beats on the master's own grid, then a reset a third of a beat in.
    const uint32_t beat = CLOCK_SUBTICKS_PER_QUARTER;
    const uint32_t reset_at = 4 * beat + beat / 3;
    uint32_t rises_before = 0, rises_after = 0, first_after = 0;
    bool was = false;
    uint32_t now = 0;
    for (uint32_t t = 0; t < 8 * beat; t++) {
        gpio.set_input(7, t == reset_at ? GPIO_HIGH : GPIO_LOW);
        master.clock().advance();
        master.pass(now);
        const bool high = gpio.outputs[0] == GPIO_HIGH;
        if (high && !was) {
            if (t <= reset_at) rises_before++;
            else { if (!rises_after) first_after = t; rises_after++; }
        }
        was = high;
        now += 1000;
    }
    // Four on the original grid. The master's count runs one subtick ahead of
    // the loop index - the pass reads the clock after advancing it - so the
    // node joins at subtick 1 and the downbeat at 0 is already past; the
    // pulses land on subticks 576, 1152, 1728 and 2304, the last of them a
    // third of a beat before the reset.
    TEST_ASSERT_EQUAL_UINT32(4, rises_before);
    // The reset itself is a downbeat, within a pass or two of the edge, and
    // the grid it starts is a beat apart from there.
    TEST_ASSERT_UINT32_WITHIN(3, reset_at, first_after);
    TEST_ASSERT_EQUAL_UINT32(4, rises_after);
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
    RUN_TEST(test_every_note_value_is_a_whole_number_of_subticks);
    RUN_TEST(test_a_quarter_note_is_a_divider_of_ppqn);
    RUN_TEST(test_triplets_and_dots_land_where_they_are_named);
    RUN_TEST(test_a_metronome_loaded_late_lands_on_the_beat);
    RUN_TEST(test_changing_the_division_keeps_the_rate_exact);
    RUN_TEST(test_reset_re_anchors_the_grid);
    return UNITY_END();
}
