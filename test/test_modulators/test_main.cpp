#include <unity.h>
#include <stdlib.h>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "bus/bus_manager.h"
#include "master.h"
#include "node/patch.h"
#include "node/registry.h"
#include "control/cc_mapper.h"
#include "control/mod_matrix.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "algorithm/util/gate_hold.h"
#include "algorithm/modulator/lfo.h"
#include "algorithm/modulator/sample_hold.h"
#include "algorithm/modulator/slew.h"
#include "algorithm/clock/clock_div.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// Utility modules and modulation: the nodes that hold a gate up and the ones
// that produce a control signal, plus the matrix that turns a control signal
// into a parameter write.

// One pass around a node, as MixedModeMaster::pass does it: what was written
// last time is published, the node runs, and its writes are published in turn.
template <class T>
static void pass(T& node, BusManager& bus, uint32_t now_us){
    node.process(bus, now_us);
    bus.swap();
}

// ---------------------------------------------------------------------------
// GateHold
// ---------------------------------------------------------------------------

static NodeConfig hold_config(uint8_t mode, uint8_t hold_ms, bool with_reset){
    NodeConfig c = node_config(ALGO_GATE_HOLD);
    c.in_bus[0] = 0;                                  // set
    if (with_reset) c.in_bus[1] = 1;                  // reset
    c.out_bus[0] = 2;
    c.params[0] = mode;
    c.params[1] = hold_ms;
    return c;
}

// Drives the `set` inlet for one pass and reports what the outlet held.
static bool run_hold(GateHold& node, BusManager& bus, bool set_level,
                     bool reset_level, uint32_t now_us){
    bus.gate_write(0, set_level);
    bus.gate_write(1, reset_level);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    return bus.gate_read(2);
}

static void test_latch_holds_a_trigger_up_for_ever() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_LATCH, 0, true);
    GateHold node(c);

    TEST_ASSERT_FALSE(run_hold(node, bus, false, false, 0));
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 1000));      // the edge
    TEST_ASSERT_TRUE(run_hold(node, bus, false, false, 2000));     // the trigger is gone
    // A trigger is 5 ms wide; this is ten seconds later and still up, which is
    // the whole point of the node.
    for (uint32_t t = 3000; t < 10000000u; t += 100000u){
        TEST_ASSERT_TRUE(run_hold(node, bus, false, false, t));
    }
    TEST_ASSERT_FALSE(run_hold(node, bus, false, true, 10100000u));
}

static void test_a_latch_with_no_reset_inlet_still_holds() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_LATCH, 0, false);
    GateHold node(c);
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 0));
    TEST_ASSERT_TRUE(run_hold(node, bus, false, true, 1000));      // nothing reads bus 1
}

static void test_toggle_flips_on_every_edge() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_TOGGLE, 0, true);
    GateHold node(c);
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 0));
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 1000));      // still the same edge
    TEST_ASSERT_TRUE(run_hold(node, bus, false, false, 2000));
    TEST_ASSERT_FALSE(run_hold(node, bus, true, false, 3000));     // the second edge
    TEST_ASSERT_FALSE(run_hold(node, bus, false, false, 4000));
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 5000));
    TEST_ASSERT_FALSE(run_hold(node, bus, true, true, 6000));      // reset wins
}

static void test_extend_turns_a_trigger_into_a_gate() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_EXTEND, 50, false);
    GateHold node(c);
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 0));
    TEST_ASSERT_TRUE(run_hold(node, bus, false, false, 5000));     // the trigger ended
    TEST_ASSERT_TRUE(run_hold(node, bus, false, false, 49000));
    TEST_ASSERT_FALSE(run_hold(node, bus, false, false, 50000));   // 50 ms exactly
}

static void test_extend_retriggers_rather_than_chopping() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_EXTEND, 50, false);
    GateHold node(c);
    run_hold(node, bus, true, false, 0);
    run_hold(node, bus, false, false, 10000);
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 40000));     // a second trigger
    TEST_ASSERT_TRUE(run_hold(node, bus, false, false, 80000));    // 40 ms after it
    TEST_ASSERT_FALSE(run_hold(node, bus, false, false, 91000));
}

static void test_limit_cuts_a_long_gate_and_needs_a_new_edge() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_LIMIT, 20, false);
    GateHold node(c);
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 0));
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 19000));
    TEST_ASSERT_FALSE(run_hold(node, bus, true, false, 20000));
    // Still held down: the output does not come back without a new edge.
    TEST_ASSERT_FALSE(run_hold(node, bus, true, false, 60000));
    TEST_ASSERT_FALSE(run_hold(node, bus, false, false, 61000));
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 62000));
}

// The switch: nothing patched at all, and the parameter is the gate. This is
// what a patch needs when something has to be held open and there is no
// trigger anywhere that should be doing it.
static void test_the_gate_parameter_holds_a_level_with_nothing_patched() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_GATE_HOLD);
    c.out_bus[0] = 2;                                 // both inlets stay NO_BUS
    c.params[0] = GateHold::HOLD_LATCH;
    c.params[GateHold::P_GATE] = 1;                   // stored up: it loads up
    GateHold node(c);

    // Ten seconds of passes with nothing driving it, and it is still there.
    for (uint32_t t = 0; t < 10000000u; t += 100000u){
        bus.swap();
        node.process(bus, t);
        bus.swap();
        TEST_ASSERT_TRUE(bus.gate_read(2));
    }
    TEST_ASSERT_EQUAL(1, node.get_param(GateHold::P_GATE));

    // And down again, from the same one control.
    TEST_ASSERT_TRUE(node.set_param(GateHold::P_GATE, 0));
    bus.swap();
    node.process(bus, 10100000u);
    bus.swap();
    TEST_ASSERT_FALSE(bus.gate_read(2));
    TEST_ASSERT_EQUAL(0, node.get_param(GateHold::P_GATE));
}

// A switch and a cable are one control, not two: either moves the level the
// other reads back.
static void test_the_switch_and_the_inlets_move_the_same_level() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_LATCH, 0, true);
    GateHold node(c);

    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 0));          // set edge
    TEST_ASSERT_EQUAL(1, node.get_param(GateHold::P_GATE));         // the switch agrees
    TEST_ASSERT_TRUE(node.set_param(GateHold::P_GATE, 0));          // put it down by hand
    TEST_ASSERT_FALSE(run_hold(node, bus, false, false, 1000));
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 2000));       // a new edge raises it
    TEST_ASSERT_FALSE(run_hold(node, bus, false, true, 3000));      // reset still drops it
    TEST_ASSERT_EQUAL(0, node.get_param(GateHold::P_GATE));
}

static void test_a_mode_change_does_not_drop_a_held_gate() {
    BusManager bus;
    NodeConfig c = hold_config(GateHold::HOLD_LATCH, 0, false);
    GateHold node(c);
    TEST_ASSERT_TRUE(run_hold(node, bus, true, false, 0));
    TEST_ASSERT_TRUE(node.set_param(0, GateHold::HOLD_TOGGLE));
    TEST_ASSERT_TRUE(run_hold(node, bus, false, false, 1000));
    TEST_ASSERT_FALSE(run_hold(node, bus, true, false, 2000));     // the toggle's edge
}

// ---------------------------------------------------------------------------
// LFO
// ---------------------------------------------------------------------------

static NodeConfig lfo_config(uint8_t shape, uint8_t sync, uint8_t rate, uint8_t polarity){
    NodeConfig c = node_config(ALGO_LFO);
    c.out_bus[0] = 0;
    c.params[0] = shape;
    c.params[1] = sync;
    c.params[2] = rate;
    c.params[8] = polarity;
    return c;
}

static void test_a_free_ramp_covers_full_scale_over_its_cycle() {
    BusManager bus;
    // 1 Hz, unipolar: one cycle is a million microseconds.
    NodeConfig c = lfo_config(Lfo::LFO_RAMP_UP, Lfo::LFO_FREE, 10, Lfo::LFO_UNIPOLAR);
    Lfo node(c);

    int16_t lowest = CV_MAX;
    int16_t highest = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        pass(node, bus, t);
        const int16_t v = bus.cv_read(0);
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    // The whole twelve bits, not a seven-bit fraction of them: that is the
    // resolution the modulation matrix has to work with.
    TEST_ASSERT_LESS_THAN_INT16(64, lowest);
    TEST_ASSERT_GREATER_THAN_INT16(CV_MAX - 64, highest);
}

static void test_a_free_lfo_completes_one_cycle_per_period() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_RAMP_UP, Lfo::LFO_FREE, 10, Lfo::LFO_UNIPOLAR);
    Lfo node(c);
    uint32_t wraps = 0;
    int16_t last = 0;
    // The accumulator's increment is truncated, which makes a cycle up to
    // 0.12% long - so this runs a little past four seconds rather than
    // landing exactly on the fourth wrap.
    for (uint32_t t = 0; t <= 4100000u; t += 500u){
        pass(node, bus, t);
        const int16_t v = bus.cv_read(0);
        if (v < last - CV_HALF) wraps++;
        last = v;
    }
    // Four seconds at 1 Hz. The accumulator truncates by up to 0.12%, which
    // over four cycles is far less than one.
    TEST_ASSERT_EQUAL_UINT32(4, wraps);
}

static void test_a_bipolar_sine_leaves_zero_rising_and_reaches_both_rails() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_SINE, Lfo::LFO_FREE, 10, Lfo::LFO_BIPOLAR);
    Lfo node(c);
    pass(node, bus, 0);
    TEST_ASSERT_INT16_WITHIN(2, 0, bus.cv_read(0));       // starts centred

    int16_t lowest = CV_MAX;
    int16_t highest = -CV_MAX;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        pass(node, bus, t);
        const int16_t v = bus.cv_read(0);
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    TEST_ASSERT_INT16_WITHIN(40, CV_HALF - 1, highest);
    TEST_ASSERT_INT16_WITHIN(40, -CV_HALF, lowest);

    // And it went up before it went down.
    Lfo fresh(c);
    pass(fresh, bus, 0);
    pass(fresh, bus, 100000u);                            // a tenth of the way
    TEST_ASSERT_GREATER_THAN_INT16(0, bus.cv_read(0));
}

static void test_depth_and_offset_scale_the_shape() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_RAMP_UP, Lfo::LFO_FREE, 10, Lfo::LFO_UNIPOLAR);
    c.params[5] = 128;                                    // half depth
    Lfo node(c);
    int16_t highest = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        pass(node, bus, t);
        if (bus.cv_read(0) > highest) highest = bus.cv_read(0);
    }
    TEST_ASSERT_INT16_WITHIN(40, CV_MAX * 128 / 255, highest);
}

static void test_a_clock_synced_lfo_runs_one_cycle_per_division() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_RAMP_UP, Lfo::LFO_CLOCK, 0, Lfo::LFO_UNIPOLAR);
    c.params[3] = DIV_QUARTER;
    c.params[4] = FEEL_STRAIGHT;
    Lfo node(c);

    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER, node.period_subticks());

    // Half a quarter note in: half way up the ramp. The node has never seen a
    // tick before this one, and still lands where the *clock's* grid says -
    // the cycle is anchored on subtick zero, not on when the node started.
    node.tick(bus, CLOCK_SUBTICKS_PER_QUARTER / 2u);
    TEST_ASSERT_INT16_WITHIN(8, CV_HALF, (int16_t)node.phase());
    // A whole one: back to the bottom.
    node.tick(bus, CLOCK_SUBTICKS_PER_QUARTER);
    TEST_ASSERT_INT16_WITHIN(8, 0, (int16_t)node.phase());
}

static void test_a_triplet_lfo_is_a_whole_number_of_subticks() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_TRIANGLE, Lfo::LFO_CLOCK, 0, Lfo::LFO_BIPOLAR);
    c.params[3] = DIV_EIGHTH;
    c.params[4] = FEEL_TRIPLET;
    Lfo node(c);
    (void)bus;
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 3u, node.period_subticks());
}

static void test_a_reset_edge_restarts_the_cycle() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_RAMP_UP, Lfo::LFO_FREE, 10, Lfo::LFO_UNIPOLAR);
    c.in_bus[0] = 0;                                      // reset, on gate bus 0
    Lfo node(c);
    for (uint32_t t = 0; t <= 500000u; t += 1000u) pass(node, bus, t);
    TEST_ASSERT_GREATER_THAN_INT16(CV_HALF - 200, bus.cv_read(0));

    bus.gate_write(0, true);
    bus.swap();
    node.process(bus, 501000u);
    bus.swap();
    TEST_ASSERT_LESS_THAN_INT16(64, bus.cv_read(0));
}

static void test_a_rate_change_does_not_jump_the_phase() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_RAMP_UP, Lfo::LFO_FREE, 10, Lfo::LFO_UNIPOLAR);
    Lfo node(c);
    for (uint32_t t = 0; t <= 400000u; t += 1000u) pass(node, bus, t);
    const int16_t before = bus.cv_read(0);
    TEST_ASSERT_TRUE(node.set_param(2, 20));              // twice as fast
    pass(node, bus, 401000u);
    // Continuous: the next value is a step onwards, not a jump home.
    TEST_ASSERT_INT16_WITHIN(64, before, bus.cv_read(0));
}

static void test_a_random_step_lfo_holds_one_level_per_cycle() {
    BusManager bus;
    NodeConfig c = lfo_config(Lfo::LFO_RANDOM_STEP, Lfo::LFO_FREE, 10, Lfo::LFO_UNIPOLAR);
    Lfo node(c);
    pass(node, bus, 0);
    const int16_t held = bus.cv_read(0);
    for (uint32_t t = 1000; t < 900000u; t += 1000u){
        pass(node, bus, t);
        TEST_ASSERT_EQUAL_INT16(held, bus.cv_read(0));
    }
    // The next cycle draws again. A draw can repeat a level, so this asks for
    // a change over several cycles rather than over one.
    bool changed = false;
    for (uint32_t t = 900000u; t < 6000000u; t += 1000u){
        pass(node, bus, t);
        if (bus.cv_read(0) != held) changed = true;
    }
    TEST_ASSERT_TRUE(changed);
}

// ---------------------------------------------------------------------------
// SampleHold
// ---------------------------------------------------------------------------

static NodeConfig sh_config(uint8_t source, uint8_t mode, uint8_t steps, bool with_signal){
    NodeConfig c = node_config(ALGO_SAMPLE_HOLD);
    c.in_bus[0] = 0;                                      // trigger, gate bus 0
    if (with_signal) c.in_bus[1] = 1;                     // signal, CV bus 1
    c.out_bus[0] = 2;
    c.params[0] = source;
    c.params[1] = mode;
    c.params[2] = steps;
    return c;
}

// One pass with the trigger and the signal set.
static int16_t run_sh(SampleHold& node, BusManager& bus, bool trigger, int16_t signal){
    bus.gate_write(0, trigger);
    bus.cv_write(1, signal);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    return bus.cv_read(2);
}

static void test_sample_and_hold_holds_the_value_at_the_edge() {
    BusManager bus;
    NodeConfig c = sh_config(SampleHold::SH_AUTO, SampleHold::SH_SAMPLE, 0, true);
    SampleHold node(c);
    TEST_ASSERT_EQUAL_INT16(0, run_sh(node, bus, false, 1000));
    TEST_ASSERT_EQUAL_INT16(1000, run_sh(node, bus, true, 1000));
    // The signal moves, the trigger does not: the held value does not move.
    TEST_ASSERT_EQUAL_INT16(1000, run_sh(node, bus, true, 3000));
    TEST_ASSERT_EQUAL_INT16(1000, run_sh(node, bus, false, 3000));
    TEST_ASSERT_EQUAL_INT16(3000, run_sh(node, bus, true, 3000));
    TEST_ASSERT_EQUAL_UINT32(2, node.samples());
}

static void test_track_and_hold_follows_while_the_gate_is_up() {
    BusManager bus;
    NodeConfig c = sh_config(SampleHold::SH_AUTO, SampleHold::SH_TRACK, 0, true);
    SampleHold node(c);
    TEST_ASSERT_EQUAL_INT16(500, run_sh(node, bus, true, 500));
    TEST_ASSERT_EQUAL_INT16(900, run_sh(node, bus, true, 900));
    TEST_ASSERT_EQUAL_INT16(900, run_sh(node, bus, false, 4000));   // frozen
}

static void test_an_unpatched_signal_inlet_samples_noise() {
    BusManager bus;
    NodeConfig c = sh_config(SampleHold::SH_AUTO, SampleHold::SH_SAMPLE, 0, false);
    SampleHold node(c);
    bool differed = false;
    int16_t first = -1;
    for (uint8_t i = 0; i < 16; i++){
        const int16_t v = run_sh(node, bus, true, 0);
        run_sh(node, bus, false, 0);
        if (first < 0) first = v;
        else if (v != first) differed = true;
        TEST_ASSERT_TRUE(v >= 0 && v <= CV_MAX);
    }
    TEST_ASSERT_TRUE(differed);
}

static void test_steps_quantise_the_held_level_and_requantise_on_edit() {
    BusManager bus;
    NodeConfig c = sh_config(SampleHold::SH_AUTO, SampleHold::SH_SAMPLE, 2, true);
    SampleHold node(c);
    // Two levels: the bottom and the top, nearest wins.
    TEST_ASSERT_EQUAL_INT16(0, run_sh(node, bus, true, 1000));
    run_sh(node, bus, false, 0);
    TEST_ASSERT_EQUAL_INT16(CV_MAX, run_sh(node, bus, true, 3000));
    // Refining the control acts on the level already held, from the reading
    // it came from - not on the quantised value, which would compound.
    TEST_ASSERT_TRUE(node.set_param(2, 0));
    TEST_ASSERT_EQUAL_INT16(3000, node.value());
}

// ---------------------------------------------------------------------------
// Slew
// ---------------------------------------------------------------------------

static void test_slew_takes_the_first_reading_whole() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_SLEW);
    c.in_bus[0] = 0;
    c.out_bus[0] = 1;
    c.params[0] = 100;                                    // one second per full scale
    Slew node(c);
    bus.cv_write(0, 2000);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    TEST_ASSERT_EQUAL_INT16(2000, bus.cv_read(1));
}

static void test_slew_takes_its_time_and_arrives() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_SLEW);
    c.in_bus[0] = 0;
    c.out_bus[0] = 1;
    c.params[0] = 100;                                    // one second, full scale
    c.params[1] = 100;
    Slew node(c);
    bus.cv_write(0, 0);
    bus.swap();
    node.process(bus, 0);
    bus.swap();

    // Step to full scale. A second later it should be there, and half a
    // second in it should be about half way.
    for (uint32_t t = 1000; t <= 500000u; t += 1000u){
        bus.cv_write(0, CV_MAX);
        bus.swap();
        node.process(bus, t);
        bus.swap();
    }
    TEST_ASSERT_INT16_WITHIN(120, CV_HALF, bus.cv_read(1));
    for (uint32_t t = 501000u; t <= 1100000u; t += 1000u){
        bus.cv_write(0, CV_MAX);
        bus.swap();
        node.process(bus, t);
        bus.swap();
    }
    TEST_ASSERT_EQUAL_INT16(CV_MAX, bus.cv_read(1));
}

static void test_rise_and_fall_are_separate() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_SLEW);
    c.in_bus[0] = 0;
    c.out_bus[0] = 1;
    c.params[0] = 0;                                      // instant up
    c.params[1] = 100;                                    // one second down
    Slew node(c);
    bus.cv_write(0, 0);
    bus.swap(); node.process(bus, 0); bus.swap();

    bus.cv_write(0, CV_MAX);
    bus.swap(); node.process(bus, 1000); bus.swap();
    TEST_ASSERT_EQUAL_INT16(CV_MAX, bus.cv_read(1));      // straight there

    for (uint32_t t = 2000; t <= 100000u; t += 1000u){
        bus.cv_write(0, 0);
        bus.swap(); node.process(bus, t); bus.swap();
    }
    // A tenth of a second down a one-second fall: a tenth of the way.
    TEST_ASSERT_INT16_WITHIN(150, CV_MAX - CV_MAX / 10, bus.cv_read(1));
}

// ---------------------------------------------------------------------------
// The modulation matrix
// ---------------------------------------------------------------------------

struct Rig {
    FakeGpio gpio;
    RecordingMidiOut midi;
    FakeEeprom eeprom;
    FakeLeds led_driver;
    MixedModeMaster master;
    StatusLeds leds;
    PatchStore store;
    PatchManager patches;
    CcMapper cc;
    ModMatrix mod;

    Rig() : gpio(), midi(), eeprom(), led_driver(),
            master(gpio, midi), leds(led_driver), store(eeprom),
            patches(master, store, leds), cc(patches, master), mod(patches, cc) {}

    // One main-loop turn, in main.cpp's order: the controllers, then the
    // modulation, then the pass.
    void turn(uint32_t now_us){
        cc.apply(now_us);
        mod.apply(master.buses(), now_us);
        master.pass(now_us);
    }
};

// A patch of one LFO writing CV bus 0 and one ClockDiv to modulate.
static Patch modulation_patch(uint8_t lfo_shape = Lfo::LFO_RAMP_UP,
                             uint8_t polarity = Lfo::LFO_UNIPOLAR){
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_LFO);
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[0] = lfo_shape;
    p.nodes[0].params[1] = Lfo::LFO_FREE;
    p.nodes[0].params[2] = 10;                            // 1 Hz
    p.nodes[0].params[8] = polarity;
    p.nodes[1] = node_config(ALGO_CLOCK_DIV);
    p.nodes[1].out_bus[0] = 0;                            // a gate bus
    p.nodes[1].params[1] = 4;                             // amount
    p.n_nodes = 2;
    return p;
}

static ModRoute route_to(uint8_t node, uint16_t param, uint8_t flags, uint8_t depth = 255){
    ModRoute r = unused_route();
    r.bus = 0;
    r.target_kind = CC_TARGET_NODE;
    r.target_index = node;
    r.param = param;
    r.depth = depth;
    r.flags = flags;
    return r;
}

static void test_an_absolute_route_sweeps_the_whole_range() {
    Rig rig;
    Patch p = modulation_patch();
    // ClockDiv's amount is 1..255; an absolute route at full depth should
    // reach both ends of that over one LFO cycle.
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    uint8_t lowest = 255;
    uint8_t highest = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        TEST_ASSERT_TRUE(rig.master.get_node_param(1, 1, v));
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(4, lowest);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(250, highest);
}

static void test_an_offset_route_swings_around_the_value_that_is_set() {
    Rig rig;
    // A bipolar route reads a signal centred on zero, so the modulator has
    // to be producing one: pairing a unipolar shape with a bipolar route
    // would give the route only the half of its swing above the centre.
    Patch p = modulation_patch(Lfo::LFO_RAMP_UP, Lfo::LFO_BIPOLAR);
    p.nodes[1].params[1] = 100;                           // the set point
    // A bipolar offset covers half the *route's* range each way, scaled by
    // depth: 100/255 of half of 1..255 is about fifty either side of 100.
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR), 100);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    uint8_t lowest = 255;
    uint8_t highest = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        TEST_ASSERT_TRUE(rig.master.get_node_param(1, 1, v));
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    // Around 100, not from zero: the set point is still the set point, and
    // the modulation is a swing about it rather than a replacement for it.
    TEST_ASSERT_UINT8_WITHIN(6, 50, lowest);
    TEST_ASSERT_UINT8_WITHIN(6, 150, highest);
}

static void test_a_knob_moves_the_centre_an_offset_route_swings_around() {
    Rig rig;
    Patch p = modulation_patch(Lfo::LFO_RAMP_UP, Lfo::LFO_BIPOLAR);
    p.nodes[1].params[1] = 60;
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR), 40);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    uint32_t t = 0;
    uint8_t lowest = 255, highest = 0;
    for (; t <= 1000000u; t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        rig.master.get_node_param(1, 1, v);
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    TEST_ASSERT_TRUE(lowest >= 40 && highest <= 90);

    // Somebody sets the parameter to 200 - an editor, a knob, the console. The
    // modulation must follow the new centre rather than dragging it back.
    TEST_ASSERT_EQUAL(PARAM_SET_OK, rig.patches.set_param(1, 1, 200, t));
    lowest = 255; highest = 0;
    for (uint32_t k = 0; k <= 1000000u; k += 1000u, t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        rig.master.get_node_param(1, 1, v);
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    TEST_ASSERT_TRUE(lowest >= 180);
    TEST_ASSERT_TRUE(highest <= 230);
}

static void test_depth_scales_how_far_a_route_reaches() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE, 64);      // a quarter
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    uint8_t highest = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        rig.master.get_node_param(1, 1, v);
        if (v > highest) highest = v;
    }
    TEST_ASSERT_UINT8_WITHIN(8, 1 + 254 * 64 / 255, highest);
}

static void test_invert_turns_the_signal_over() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_ABSOLUTE | MOD_INVERT));
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    // A rising ramp, inverted, starts at the top.
    rig.turn(0);
    rig.turn(1000);
    uint8_t v = 0;
    rig.master.get_node_param(1, 1, v);
    TEST_ASSERT_GREATER_THAN_UINT8(240, v);
}

static void test_a_zero_depth_route_writes_nothing() {
    Rig rig;
    Patch p = modulation_patch();
    p.nodes[1].params[1] = 7;
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE, 0);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    for (uint32_t t = 0; t <= 100000u; t += 1000u) rig.turn(t);
    uint8_t v = 0;
    rig.master.get_node_param(1, 1, v);
    TEST_ASSERT_EQUAL_UINT8(7, v);
    TEST_ASSERT_EQUAL_UINT32(0, rig.mod.writes());
}

static void test_a_route_writes_once_a_pass_at_most() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    for (uint32_t t = 0; t < 100000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(100, rig.mod.writes());
}

static void test_an_unchanged_value_costs_no_write() {
    Rig rig;
    Patch p = modulation_patch(Lfo::LFO_RANDOM_STEP);      // one level per cycle
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    for (uint32_t t = 0; t < 500000u; t += 1000u) rig.turn(t);
    // Five hundred passes, one held level: a handful of writes, not five
    // hundred.
    TEST_ASSERT_LESS_THAN_UINT32(10, rig.mod.writes());
}

static void test_two_routes_on_one_parameter_are_refused() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    p.mod_map[1] = route_to(1, 1, MOD_ABSOLUTE);
    p.mod_map[1].bus = 1;
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    TEST_ASSERT_EQUAL(LOAD_MOD_ROUTE_INVALID, rig.master.last_error());
}

static void test_a_route_to_a_parameter_that_does_not_exist_is_refused() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 900, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    p.mod_map[0] = route_to(9, 0, MOD_ABSOLUTE);           // no such node
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    p.mod_map[0].bus = N_CV_BUS;                           // no such bus
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
}

static void test_a_route_cannot_press_the_transport() {
    Rig rig;
    Patch p = modulation_patch();
    ModRoute r = unused_route();
    r.bus = 0;
    r.target_kind = CC_TARGET_TRANSPORT;
    r.param = CC_TRANSPORT_START;
    r.depth = 255;
    p.mod_map[0] = r;
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
}

static void test_a_route_can_drive_the_tempo() {
    Rig rig;
    Patch p = modulation_patch();
    ModRoute r = unused_route();
    r.bus = 0;
    r.target_kind = CC_TARGET_CLOCK;
    r.param = CC_CLOCK_TEMPO;
    r.depth = 255;
    r.flags = MOD_ABSOLUTE;
    p.mod_map[0] = r;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    uint16_t lowest = CLOCK_MAX_BPM;
    uint16_t highest = CLOCK_MIN_BPM;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        rig.turn(t);
        const uint16_t bpm = rig.master.clock().bpm();
        if (bpm < lowest) lowest = bpm;
        if (bpm > highest) highest = bpm;
    }
    TEST_ASSERT_LESS_THAN_UINT16(CLOCK_MIN_BPM + 10, lowest);
    TEST_ASSERT_GREATER_THAN_UINT16(CLOCK_MAX_BPM - 10, highest);
}

static void test_editing_a_route_forgets_the_centre_it_was_holding() {
    Rig rig;
    Patch p = modulation_patch();
    p.nodes[1].params[1] = 100;
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR), 40);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    for (uint32_t t = 0; t < 200000u; t += 1000u) rig.turn(t);

    // Point the same slot at a different parameter. The centre it was holding
    // belonged to the old target and must not be applied to the new one.
    rig.patches.begin_edit();
    rig.patches.staging().mod_map[0] = route_to(1, 2, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR), 40);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.commit_mod_route(0, 200000u));
    for (uint32_t t = 200000u; t < 400000u; t += 1000u) rig.turn(t);
    uint8_t phase = 0;
    rig.master.get_node_param(1, 2, phase);
    // ClockDiv's phase started at 0 and is being pushed around zero, so it
    // stays near the bottom of its range rather than near 100.
    TEST_ASSERT_LESS_THAN_UINT8(80, phase);
}

static void test_routes_round_trip_through_the_codec() {
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR | MOD_INVERT), 200);
    p.mod_map[0].min = 10;
    p.mod_map[0].max = 90;
    p.mod_map[5] = route_to(1, 2, MOD_ABSOLUTE, 128);
    p.mod_map[5].bus = 3;

    static uint8_t image[2048];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK,
        patch_codec::encode(p, default_globals(), image, sizeof image, written));

    Patch back = empty_patch();
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(image, written, back, g));
    TEST_ASSERT_EQUAL_UINT8(0, back.mod_map[0].bus);
    TEST_ASSERT_EQUAL_UINT16(1, back.mod_map[0].param);
    TEST_ASSERT_EQUAL_UINT8(200, back.mod_map[0].depth);
    TEST_ASSERT_EQUAL_UINT8(MOD_OFFSET | MOD_BIPOLAR | MOD_INVERT, back.mod_map[0].flags);
    TEST_ASSERT_EQUAL_UINT16(10, back.mod_map[0].min);
    TEST_ASSERT_EQUAL_UINT16(90, back.mod_map[0].max);
    TEST_ASSERT_EQUAL_UINT8(3, back.mod_map[5].bus);
    TEST_ASSERT_EQUAL_UINT8(128, back.mod_map[5].depth);
    // Every other slot is unused, and stays that way.
    TEST_ASSERT_EQUAL_UINT8(NO_BUS, back.mod_map[1].bus);
}

static void test_a_patch_written_before_modulation_still_decodes() {
    // A version 2 image: this format with no route block at the tail. Built by
    // encoding, then re-stamping the version and trimming the trailing zero
    // route count, so it is the bytes an older firmware really wrote.
    Patch p = modulation_patch();
    static uint8_t image[2048];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK,
        patch_codec::encode(p, default_globals(), image, sizeof image, written));

    // Drop the route-count byte and the CRC, restamp as version 2, re-CRC.
    const size_t payload = (size_t)(image[6] | ((size_t)image[7] << 8)) - 1u;
    image[4] = 2;
    image[6] = (uint8_t)(payload & 0xFF);
    image[7] = (uint8_t)(payload >> 8);
    const size_t crc_at = 8 + payload;
    const uint16_t crc = patch_codec::crc16(image, crc_at);
    image[crc_at] = (uint8_t)(crc & 0xFF);
    image[crc_at + 1] = (uint8_t)(crc >> 8);

    Patch back = empty_patch();
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(image, crc_at + 2, back, g));
    TEST_ASSERT_EQUAL_UINT8(2, back.n_nodes);
    TEST_ASSERT_EQUAL_UINT8(NO_BUS, back.mod_map[0].bus);
}

// A modulator writes every pass for as long as the patch runs. If those
// writes went through the ordinary edit path they would re-arm the autosave
// timer on every one of them, and slot 0 would be rewritten to EEPROM every
// couple of seconds until the module was switched off.
static void test_a_running_modulator_does_not_wear_out_the_eeprom() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    // The load itself marks the store dirty and the debounce writes it once.
    uint32_t t = 0;
    for (; t < 10000000u; t += 1000u){
        rig.turn(t);
        rig.patches.service(t);
    }
    const uint32_t after_ten_seconds = rig.store.writes();

    for (; t < 60000000u; t += 1000u){
        rig.turn(t);
        rig.patches.service(t);
    }
    // Fifty more seconds of modulation, and not one further write.
    TEST_ASSERT_EQUAL_UINT32(after_ten_seconds, rig.store.writes());
    TEST_ASSERT_FALSE(rig.patches.active().nodes[1].params[1] == 0
                      && rig.mod.writes() == 0);
}

// The stored value is the set point a user chose. What the parameter happens
// to hold right now is the modulator's, and saving that would mean a preset
// captured whatever phase the LFO was at - which an offset route would then
// take as its new centre, walking the parameter away a save at a time.
static void test_the_patch_image_keeps_the_set_point_not_the_modulation() {
    Rig rig;
    Patch p = modulation_patch(Lfo::LFO_RAMP_UP, Lfo::LFO_BIPOLAR);
    p.nodes[1].params[1] = 100;
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR), 100);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    for (uint32_t t = 0; t < 400000u; t += 1000u) rig.turn(t);

    uint8_t running = 0;
    TEST_ASSERT_TRUE(rig.master.get_node_param(1, 1, running));
    TEST_ASSERT_NOT_EQUAL(100, running);                  // it is being modulated
    TEST_ASSERT_EQUAL_UINT8(100, rig.patches.active().nodes[1].params[1]);
}

// A grid centred on zero, so a bipolar signal is quantised as evenly as a
// unipolar one - a grid laid out from zero upwards would flatten a bipolar
// LFO's whole negative half onto one level.
static void test_quantising_keeps_a_bipolar_signal_bipolar() {
    BusManager bus;
    NodeConfig c = sh_config(SampleHold::SH_AUTO, SampleHold::SH_SAMPLE, 5, true);
    SampleHold node(c);
    const int16_t step = CV_MAX / 4;
    const struct { int16_t in; int16_t out; } cases[] = {
        {0, 0}, {step, step}, {-step, -step},
        {(int16_t)(step - 3), step}, {(int16_t)(-step + 3), (int16_t)-step},
        {-2048, (int16_t)(-2 * step)}, {2047, (int16_t)(2 * step)},
    };
    for (const auto& k : cases){
        TEST_ASSERT_EQUAL_INT16(k.out, run_sh(node, bus, true, k.in));
        run_sh(node, bus, false, 0);
    }
}

// A unipolar source read through a bipolar route - which is every route made
// by dragging SampleHold, Slew or a unipolar LFO onto a block - must not lose
// the top half of its travel to a clamp at CV_HALF.
static void test_a_unipolar_signal_read_as_bipolar_keeps_its_whole_travel() {
    Rig rig;
    Patch p = modulation_patch(Lfo::LFO_RAMP_UP, Lfo::LFO_UNIPOLAR);
    p.nodes[1].params[1] = 1;
    p.mod_map[0] = route_to(1, 1, (uint8_t)(MOD_OFFSET | MOD_BIPOLAR));
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    uint8_t highest = 0;
    uint16_t distinct = 0;
    uint8_t last = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        rig.master.get_node_param(1, 1, v);
        if (v != last){ distinct++; last = v; }
        if (v > highest) highest = v;
    }
    // The signal only ever pushes upwards - it never goes below zero - and it
    // reaches the top of the range rather than stopping half way.
    TEST_ASSERT_GREATER_THAN_UINT8(240, highest);
    TEST_ASSERT_GREATER_THAN_UINT16(50, distinct);
}

static void test_the_modulation_path_never_allocates() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    const size_t before = g_allocations;
    for (uint32_t t = 0; t < 200000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_EQUAL(before, g_allocations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_latch_holds_a_trigger_up_for_ever);
    RUN_TEST(test_a_latch_with_no_reset_inlet_still_holds);
    RUN_TEST(test_toggle_flips_on_every_edge);
    RUN_TEST(test_extend_turns_a_trigger_into_a_gate);
    RUN_TEST(test_extend_retriggers_rather_than_chopping);
    RUN_TEST(test_limit_cuts_a_long_gate_and_needs_a_new_edge);
    RUN_TEST(test_a_mode_change_does_not_drop_a_held_gate);
    RUN_TEST(test_the_gate_parameter_holds_a_level_with_nothing_patched);
    RUN_TEST(test_the_switch_and_the_inlets_move_the_same_level);

    RUN_TEST(test_a_free_ramp_covers_full_scale_over_its_cycle);
    RUN_TEST(test_a_free_lfo_completes_one_cycle_per_period);
    RUN_TEST(test_a_bipolar_sine_leaves_zero_rising_and_reaches_both_rails);
    RUN_TEST(test_depth_and_offset_scale_the_shape);
    RUN_TEST(test_a_clock_synced_lfo_runs_one_cycle_per_division);
    RUN_TEST(test_a_triplet_lfo_is_a_whole_number_of_subticks);
    RUN_TEST(test_a_reset_edge_restarts_the_cycle);
    RUN_TEST(test_a_rate_change_does_not_jump_the_phase);
    RUN_TEST(test_a_random_step_lfo_holds_one_level_per_cycle);

    RUN_TEST(test_sample_and_hold_holds_the_value_at_the_edge);
    RUN_TEST(test_track_and_hold_follows_while_the_gate_is_up);
    RUN_TEST(test_an_unpatched_signal_inlet_samples_noise);
    RUN_TEST(test_steps_quantise_the_held_level_and_requantise_on_edit);

    RUN_TEST(test_slew_takes_the_first_reading_whole);
    RUN_TEST(test_slew_takes_its_time_and_arrives);
    RUN_TEST(test_rise_and_fall_are_separate);

    RUN_TEST(test_an_absolute_route_sweeps_the_whole_range);
    RUN_TEST(test_an_offset_route_swings_around_the_value_that_is_set);
    RUN_TEST(test_a_knob_moves_the_centre_an_offset_route_swings_around);
    RUN_TEST(test_depth_scales_how_far_a_route_reaches);
    RUN_TEST(test_invert_turns_the_signal_over);
    RUN_TEST(test_a_zero_depth_route_writes_nothing);
    RUN_TEST(test_a_route_writes_once_a_pass_at_most);
    RUN_TEST(test_an_unchanged_value_costs_no_write);
    RUN_TEST(test_two_routes_on_one_parameter_are_refused);
    RUN_TEST(test_a_route_to_a_parameter_that_does_not_exist_is_refused);
    RUN_TEST(test_a_route_cannot_press_the_transport);
    RUN_TEST(test_a_route_can_drive_the_tempo);
    RUN_TEST(test_editing_a_route_forgets_the_centre_it_was_holding);
    RUN_TEST(test_routes_round_trip_through_the_codec);
    RUN_TEST(test_a_patch_written_before_modulation_still_decodes);
    RUN_TEST(test_a_running_modulator_does_not_wear_out_the_eeprom);
    RUN_TEST(test_the_patch_image_keeps_the_set_point_not_the_modulation);
    RUN_TEST(test_quantising_keeps_a_bipolar_signal_bipolar);
    RUN_TEST(test_a_unipolar_signal_read_as_bipolar_keeps_its_whole_travel);
    RUN_TEST(test_the_modulation_path_never_allocates);
    return UNITY_END();
}
