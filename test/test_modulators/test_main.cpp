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
#include "control/macros.h"
#include "control/cc_mapper.h"
#include "control/control_sum.h"
#include "control/mod_matrix.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "algorithm/util/gate_hold.h"
#include "algorithm/modulator/lfo.h"
#include "algorithm/modulator/sample_hold.h"
#include "algorithm/modulator/slew.h"
#include "algorithm/modulator/step_mod.h"
#include "algorithm/modulator/envelope.h"
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
    c.in_buses[0] = one_bus(0);                                  // set
    if (with_reset) c.in_buses[1] = one_bus(1);                  // reset
    c.out_buses[0] = one_bus(2);
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
    c.out_buses[0] = one_bus(2);                                 // both inlets stay unconnected
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
    c.out_buses[0] = one_bus(0);
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
    c.in_buses[0] = one_bus(0);                                      // reset, on gate bus 0
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
    c.in_buses[0] = one_bus(0);                                      // trigger, gate bus 0
    if (with_signal) c.in_buses[1] = one_bus(1);                     // signal, CV bus 1
    c.out_buses[0] = one_bus(2);
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
// StepMod
// ---------------------------------------------------------------------------

static NodeConfig step_config(uint8_t shape, uint8_t steps, uint8_t direction,
                              uint8_t polarity, bool with_reset){
    NodeConfig c = node_config(ALGO_STEP_MOD);
    c.in_buses[0] = one_bus(0);                                      // trigger, gate bus 0
    if (with_reset) c.in_buses[1] = one_bus(1);                      // reset, gate bus 1
    c.out_buses[0] = one_bus(2);
    c.params[0] = shape;
    c.params[1] = steps;
    c.params[2] = direction;
    c.params[5] = polarity;
    return c;
}

// One pass with the two inlets held at these levels.
static int16_t run_step(StepMod& node, BusManager& bus, bool trigger, bool reset){
    bus.gate_write(0, trigger);
    bus.gate_write(1, reset);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    return bus.cv_read(2);
}

// A whole trigger - up, then down again so the next one is an edge - and the
// level it moved to.
static int16_t trig(StepMod& node, BusManager& bus){
    const int16_t v = run_step(node, bus, true, false);
    run_step(node, bus, false, false);
    return v;
}

static void test_a_step_reads_the_centre_of_its_slice_of_the_shape() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod node(c);
    // Four steps of a ramp are 12.5, 37.5, 62.5 and 87.5 percent of full
    // scale. Reading the *start* of each slice would put the first step on
    // the bottom rail and leave the last one an eighth short of the top.
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16(3 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16(5 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16(7 * CV_FULL / 8, trig(node, bus));
    // A period is a number of triggers, so the fifth is the first again.
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_UINT32(5, node.triggers());
}

static void test_a_bipolar_shape_is_centred_on_zero() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_BIPOLAR, false);
    StepMod node(c);
    // The same four centres, read around zero - and symmetric about it,
    // which is the other half of what sampling centres buys.
    TEST_ASSERT_EQUAL_INT16(-3 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16(-1 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16( 1 * CV_FULL / 8, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16( 3 * CV_FULL / 8, trig(node, bus));
}

static void test_the_level_holds_between_triggers() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod node(c);
    // Before anything has clocked it, the level of step zero - not silence,
    // because a CV bus has a value every pass either way.
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, node.value());
    TEST_ASSERT_EQUAL_UINT32(0, node.triggers());

    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, trig(node, bus));
    for (uint8_t i = 0; i < 20; i++){
        TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, run_step(node, bus, false, false));
    }
    // A gate that stays up is one trigger, not one per pass: it is the edge
    // that moves the step.
    run_step(node, bus, true, false);
    run_step(node, bus, true, false);
    run_step(node, bus, true, false);
    TEST_ASSERT_EQUAL_INT16(3 * CV_FULL / 8, node.value());
    TEST_ASSERT_EQUAL_UINT32(2, node.triggers());
}

static void test_direction_walks_the_steps_backwards_and_bounces() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 4, StepEngine::SEQ_REVERSE,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod back(c);
    TEST_ASSERT_EQUAL_INT16(7 * CV_FULL / 8, trig(back, bus));
    TEST_ASSERT_EQUAL_INT16(5 * CV_FULL / 8, trig(back, bus));
    TEST_ASSERT_EQUAL_INT16(3 * CV_FULL / 8, trig(back, bus));
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, trig(back, bus));
    TEST_ASSERT_EQUAL_INT16(7 * CV_FULL / 8, trig(back, bus));

    // The step engine's own rule, which is the point of sharing it: the
    // endpoints are not repeated.
    BusManager pbus;
    NodeConfig pc = step_config(CV_SHAPE_TRIANGLE, 4, StepEngine::SEQ_PINGPONG,
                                StepMod::STEP_UNIPOLAR, false);
    StepMod bounce(pc);
    static const uint8_t WANT[8] = {0, 1, 2, 3, 2, 1, 0, 1};
    for (uint8_t i = 0; i < 8; i++){
        trig(bounce, pbus);
        TEST_ASSERT_EQUAL_UINT8(WANT[i], bounce.step());
    }
}

static void test_reset_sends_the_next_trigger_to_the_first_step() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, true);
    StepMod node(c);
    trig(node, bus);
    trig(node, bus);
    TEST_ASSERT_EQUAL_INT16(5 * CV_FULL / 8, trig(node, bus));

    // Reset alone moves no level: a step is a trigger, and reset means what
    // it means in every sequencer - the *next* one plays the first step.
    run_step(node, bus, false, true);
    TEST_ASSERT_EQUAL_INT16(5 * CV_FULL / 8, node.value());
    run_step(node, bus, false, false);
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, trig(node, bus));
}

static void test_steps_cut_the_shape_finer_and_the_edit_is_heard_at_once() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 2, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod node(c);
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 4, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16(3 * CV_FULL / 4, trig(node, bus));
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 4, trig(node, bus));

    // Sixteen steps: the step being held re-reads the shape at its new place
    // straight away rather than waiting for the next trigger (#20).
    TEST_ASSERT_TRUE(node.set_param(1, 16));
    TEST_ASSERT_EQUAL_UINT8(16, node.steps());
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 32, node.value());
}

static void test_depth_and_offset_move_the_level_already_held() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RAMP_UP, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod node(c);
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, trig(node, bus));
    // Half depth, from the shape rather than from the scaled level, so
    // coarsening and refining does not compound.
    TEST_ASSERT_TRUE(node.set_param(3, 128));
    TEST_ASSERT_EQUAL_INT16(512 * 128 / 255, node.value());
    TEST_ASSERT_TRUE(node.set_param(3, 255));
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8, node.value());
    // The offset is a signed byte as a fraction of half of full scale.
    TEST_ASSERT_TRUE(node.set_param(4, 10));
    TEST_ASSERT_EQUAL_INT16(1 * CV_FULL / 8 + 10 * CV_HALF / 128, node.value());
}

static void test_random_step_draws_a_level_on_every_trigger() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RANDOM_STEP, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod node(c);
    int16_t seen[16];
    for (uint8_t i = 0; i < 16; i++){
        seen[i] = trig(node, bus);
        TEST_ASSERT_TRUE(seen[i] >= 0 && seen[i] <= CV_MAX);
    }
    // A shape repeats every period; this does not, because it draws per
    // trigger rather than per period.
    uint8_t distinct = 0;
    for (uint8_t i = 0; i < 16; i++){
        bool first = true;
        for (uint8_t j = 0; j < i; j++) if (seen[j] == seen[i]) first = false;
        if (first) distinct++;
    }
    TEST_ASSERT_TRUE(distinct >= 8);
}

static void test_random_glide_steps_between_two_levels_over_one_period() {
    BusManager bus;
    NodeConfig c = step_config(CV_SHAPE_RANDOM_GLIDE, 4, StepEngine::SEQ_FORWARD,
                               StepMod::STEP_UNIPOLAR, false);
    StepMod node(c);
    for (uint8_t period = 0; period < 8; period++){
        int16_t v[4];
        for (uint8_t i = 0; i < 4; i++) v[i] = trig(node, bus);
        // One target per period, stepped towards from the last one, so
        // inside a period the levels only ever go one way.
        const bool up = v[3] >= v[0];
        for (uint8_t i = 1; i < 4; i++){
            if (up) TEST_ASSERT_TRUE(v[i] >= v[i - 1]);
            else    TEST_ASSERT_TRUE(v[i] <= v[i - 1]);
        }
    }
}

// ---------------------------------------------------------------------------
// Slew
// ---------------------------------------------------------------------------

static void test_slew_takes_the_first_reading_whole() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_SLEW);
    c.in_buses[0] = one_bus(0);
    c.out_buses[0] = one_bus(1);
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
    c.in_buses[0] = one_bus(0);
    c.out_buses[0] = one_bus(1);
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
    c.in_buses[0] = one_bus(0);
    c.out_buses[0] = one_bus(1);
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
    Macros macros;
    CcMapper cc;
    ControlSum sum;
    ModMatrix mod;

    Rig() : gpio(), midi(), eeprom(), led_driver(),
            master(gpio, midi), leds(led_driver), store(eeprom),
            patches(master, store, leds), macros(), cc(patches, master, macros), sum(cc), mod(patches, cc, sum) {}

    // One main-loop turn, in main.cpp's order: the controllers, then the
    // offsets gathered and summed, then the pass. An offset route does not
    // write on its own any more - it contributes, and the commit is what
    // reaches the parameter - so a turn that skipped the commit would test
    // nothing.
    void turn(uint32_t now_us){
        cc.apply(now_us);
        sum.begin();
        mod.apply(master.buses(), now_us);
        macros.expand(patches.active(), sum);
        sum.commit(now_us);
        master.pass(now_us);
    }
};

// A patch of one LFO writing CV bus 0 and one ClockDiv to modulate.
static Patch modulation_patch(uint8_t lfo_shape = Lfo::LFO_RAMP_UP,
                             uint8_t polarity = Lfo::LFO_UNIPOLAR){
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_LFO);
    p.nodes[0].out_buses[0] = one_bus(0);
    p.nodes[0].params[0] = lfo_shape;
    p.nodes[0].params[1] = Lfo::LFO_FREE;
    p.nodes[0].params[2] = 10;                            // 1 Hz
    p.nodes[0].params[8] = polarity;
    p.nodes[1] = node_config(ALGO_CLOCK_DIV);
    p.nodes[1].out_buses[0] = one_bus(0);                            // a gate bus
    p.nodes[1].params[1] = 4;                             // amount
    p.n_nodes = 2;
    return p;
}

static ModRoute route_to(uint8_t node, uint16_t param, uint8_t flags, uint8_t depth = 255){
    ModRoute r = unused_route();
    r.buses = one_bus(0);
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

// What the editor draws a live meter from. A route that does nothing looks
// exactly like a route that does nothing whatever the reason, so the matrix
// reports which reason it was - and these are the reasons, each reached the
// way a user reaches it.
static void test_a_route_says_what_it_is_doing() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    ModState s;
    // Nothing has run yet, so nothing has been decided yet.
    TEST_ASSERT_TRUE(rig.mod.state(0, s));
    TEST_ASSERT_EQUAL(MOD_STATUS_UNUSED, s.status);

    for (uint32_t t = 0; t <= 300000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_TRUE(rig.mod.state(0, s));
    TEST_ASSERT_EQUAL(MOD_STATUS_ACTIVE, s.status);
    // ClockDiv's amount is 1..255, and the route has no sub-range of its own.
    TEST_ASSERT_EQUAL_UINT16(1, s.range_lo);
    TEST_ASSERT_EQUAL_UINT16(255, s.range_hi);
    // The value it reports is the value the node is actually running.
    uint8_t running = 0;
    TEST_ASSERT_TRUE(rig.master.get_node_param(1, 1, running));
    TEST_ASSERT_EQUAL_UINT16(running, s.value);
    TEST_ASSERT_TRUE(s.position <= CV_MAX);

    // A slot nothing uses is unused, and a slot that is not a slot is not
    // answered for at all.
    TEST_ASSERT_TRUE(rig.mod.state(1, s));
    TEST_ASSERT_EQUAL(MOD_STATUS_UNUSED, s.status);
    TEST_ASSERT_FALSE(rig.mod.state(N_MOD_ROUTE, s));
}

// The three nothings, told apart. Every one of them leaves the parameter
// sitting still with a route that reads as correctly configured.
static void test_a_route_says_why_it_is_doing_nothing() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE, 0);        // depth zero
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    for (uint32_t t = 0; t <= 20000u; t += 1000u) rig.turn(t);
    ModState s;
    TEST_ASSERT_TRUE(rig.mod.state(0, s));
    TEST_ASSERT_EQUAL(MOD_STATUS_SILENT, s.status);

    // A range one value wide: the route runs, reads its signal and has
    // nowhere to put it.
    Patch pinned = modulation_patch();
    pinned.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    pinned.mod_map[0].min = 40;
    pinned.mod_map[0].max = 40;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(pinned, default_globals(), 0));
    for (uint32_t t = 0; t <= 20000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_TRUE(rig.mod.state(0, s));
    TEST_ASSERT_EQUAL(MOD_STATUS_PINNED, s.status);
    TEST_ASSERT_EQUAL_UINT16(40, s.value);

    // And the nothing the matrix cannot report, which is why the editor has
    // to say this one for itself: a bus nobody writes reads as a signal
    // sitting at zero, which is a perfectly good signal. The route is running
    // and the parameter is pinned to the bottom of its range.
    Patch nowhere = modulation_patch();
    nowhere.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    nowhere.mod_map[0].buses = one_bus(N_CV_BUS - 1);                 // the LFO writes bus 0
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(nowhere, default_globals(), 0));
    for (uint32_t t = 0; t <= 20000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_TRUE(rig.mod.state(0, s));
    TEST_ASSERT_EQUAL(MOD_STATUS_ACTIVE, s.status);
    TEST_ASSERT_EQUAL_UINT16(s.range_lo, s.value);
}

// The top of one LFO cycle, as the parameter saw it.
static uint8_t highest_over_a_cycle(Rig& rig){
    uint8_t highest = 0;
    for (uint32_t t = 0; t <= 1000000u; t += 1000u){
        rig.turn(t);
        uint8_t v = 0;
        TEST_ASSERT_TRUE(rig.master.get_node_param(1, 1, v));
        if (v > highest) highest = v;
    }
    return highest;
}

// Two routes on one parameter used to be refused, because two writers racing
// over one value has no defined result. They no longer race: an offset route
// contributes to ControlSum, which sums every contribution on a target and
// writes once. Macros forced the change - a destination that rises and then
// falls back is two windows on one parameter - and once a parameter can take
// a sum, refusing a second route would be an inconsistency rather than a
// discipline.
static void test_two_offset_routes_on_one_parameter_sum() {
    Rig rig;
    Patch p = modulation_patch();
    // Both read the same bus, so each contributes the same swing and the
    // parameter should end up twice as far from its set point as one alone.
    p.nodes[1].params[1] = 100;                            // a set point to swing around
    p.mod_map[0] = route_to(1, 1, MOD_OFFSET, 40);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    const uint8_t one = highest_over_a_cycle(rig);

    Rig both;
    Patch q = modulation_patch();
    q.nodes[1].params[1] = 100;
    q.mod_map[0] = route_to(1, 1, MOD_OFFSET, 40);
    q.mod_map[1] = route_to(1, 1, MOD_OFFSET, 40);
    TEST_ASSERT_EQUAL(APPLY_OK, both.patches.apply(q, default_globals(), 0));
    const uint8_t two = highest_over_a_cycle(both);

    // Accepted, not refused - and the second route pushed it further than the
    // first alone, which is what summing means.
    TEST_ASSERT_GREATER_THAN_UINT8(100, one);
    TEST_ASSERT_GREATER_THAN_UINT8(one, two);
}

static void test_a_route_to_a_parameter_that_does_not_exist_is_refused() {
    Rig rig;
    Patch p = modulation_patch();
    p.mod_map[0] = route_to(1, 900, MOD_ABSOLUTE);
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    p.mod_map[0] = route_to(9, 0, MOD_ABSOLUTE);           // no such node
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    p.mod_map[0] = route_to(1, 1, MOD_ABSOLUTE);
    p.mod_map[0].buses = one_bus(N_CV_BUS);                           // no such bus
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
}

static void test_a_route_cannot_press_the_transport() {
    Rig rig;
    Patch p = modulation_patch();
    ModRoute r = unused_route();
    r.buses = one_bus(0);
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
    r.buses = one_bus(0);
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
    p.mod_map[5].buses = one_bus(3);

    static uint8_t image[2048];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK,
        patch_codec::encode(p, default_globals(), image, sizeof image, written));

    Patch back = empty_patch();
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(image, written, back, g));
    TEST_ASSERT_EQUAL_UINT16(one_bus(0).bits, back.mod_map[0].buses.bits);
    TEST_ASSERT_EQUAL_UINT16(1, back.mod_map[0].param);
    TEST_ASSERT_EQUAL_UINT8(200, back.mod_map[0].depth);
    TEST_ASSERT_EQUAL_UINT8(MOD_OFFSET | MOD_BIPOLAR | MOD_INVERT, back.mod_map[0].flags);
    TEST_ASSERT_EQUAL_UINT16(10, back.mod_map[0].min);
    TEST_ASSERT_EQUAL_UINT16(90, back.mod_map[0].max);
    TEST_ASSERT_EQUAL_UINT16(one_bus(3).bits, back.mod_map[5].buses.bits);
    TEST_ASSERT_EQUAL_UINT8(128, back.mod_map[5].depth);
    // Every other slot is unused, and stays that way.
    TEST_ASSERT_FALSE(back.mod_map[1].buses.any());
}

// An image from another build carries the same fields with different meanings
// - a parameter number that moved, an algorithm id that was renumbered - and
// decoding it would write the wrong bytes into a live patch. The format
// version is what that is for: refused, and the running patch untouched.
static void test_a_patch_from_another_format_version_is_refused() {
    Patch p = modulation_patch();
    static uint8_t image[2048];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK,
        patch_codec::encode(p, default_globals(), image, sizeof image, written));

    image[4] = PATCH_FORMAT_VERSION - 1;
    const size_t payload = (size_t)(image[6] | ((size_t)image[7] << 8));
    const size_t crc_at = 8 + payload;
    const uint16_t crc = patch_codec::crc16(image, crc_at);
    image[crc_at] = (uint8_t)(crc & 0xFF);
    image[crc_at + 1] = (uint8_t)(crc >> 8);

    Patch back = empty_patch();
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(CODEC_BAD_VERSION, patch_codec::decode(image, crc_at + 2, back, g));
    TEST_ASSERT_EQUAL_UINT8(0, back.n_nodes);
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

// ---------------------------------------------------------------------------
// Envelopes
//
// A contour with a beginning. The two nodes are one walk through one set of
// stages and differ only in who ends it - an AD runs to the end by itself, an
// ADSR waits at the sustain level for as long as the gate is up - so most of
// what is checked here is checked once, on whichever of the two shows it.
// ---------------------------------------------------------------------------

// Gate bus 0 is the trigger or the gate, bus 1 the second inlet; CV bus 0 is
// the contour and gate bus 1 the end trigger.
static NodeConfig envelope_config(uint8_t algorithm, bool with_aux){
    NodeConfig c = node_config(algorithm);
    c.in_buses[0] = one_bus(0);
    if (with_aux) c.in_buses[1] = one_bus(1);
    c.out_buses[0] = one_bus(0);
    c.out_buses[1] = one_bus(1);
    return c;
}

// One pass with the inlets held where the caller wants them. Gates are
// republished every pass (the back buffer is cleared on every swap), so the
// write comes first and the node's own writes are published by the second.
static void env_pass(Node& node, BusManager& bus, uint32_t now_us, bool gate, bool aux = false){
    if (gate) bus.gate_write(0, true);
    if (aux) bus.gate_write(1, true);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
}

// Passes at a millisecond each up to `until`, holding the gate where it is.
static void env_run(Node& node, BusManager& bus, uint32_t& now_us, uint32_t until, bool gate){
    while (now_us < until){
        env_pass(node, bus, now_us, gate);
        now_us += 1000u;
    }
}

static void test_an_ad_rises_to_the_peak_and_falls_back_to_nothing() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 20;      // 200 ms
    c.params[EnvelopeNode::P_DECAY] = 20;       // 200 ms
    AdEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
    TEST_ASSERT_EQUAL_INT16(0, node.value());

    env_pass(node, bus, t, true);               // the trigger
    t += 1000u;
    // No delay and no hold by default, so the contour is in the attack on the
    // pass that fired it rather than a stage later.
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());

    env_run(node, bus, t, 40000u, false);
    TEST_ASSERT_INT16_WITHIN(150, CV_MAX / 5, node.value());
    env_run(node, bus, t, 199000u, false);
    TEST_ASSERT_GREATER_THAN_INT16(CV_MAX - 100, node.value());

    // Over the top and down the other side. A fifth of a second of decay from
    // full scale is half way down.
    env_run(node, bus, t, 300000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, node.stage());
    TEST_ASSERT_INT16_WITHIN(200, CV_HALF, node.value());

    env_run(node, bus, t, 420000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
    TEST_ASSERT_EQUAL_INT16(0, node.value());
}

static void test_the_length_of_the_trigger_means_nothing_to_an_ad() {
    BusManager short_bus;
    BusManager long_bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 14;
    c.params[EnvelopeNode::P_DECAY] = 14;
    AdEnvelope stabbed(c);
    AdEnvelope leaned_on(c);

    // One pass of trigger against a gate held the whole way through.
    for (uint32_t t = 0; t < 400000u; t += 1000u){
        const bool first = (t == 1000u);
        env_pass(stabbed, short_bus, t, first);
        env_pass(leaned_on, long_bus, t, t >= 1000u);
        TEST_ASSERT_EQUAL_INT16(stabbed.value(), leaned_on.value());
    }
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, stabbed.stage());
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, leaned_on.stage());
}

static void test_a_pre_delay_keeps_the_contour_at_nothing_first() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_DELAY] = 20;       // 200 ms
    c.params[EnvelopeNode::P_ATTACK] = 10;      // 50 ms
    AdEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_pass(node, bus, t, true);
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DELAY, node.stage());

    // Nothing at all comes out for the whole of the delay - a pre-delay that
    // leaked a level would be an attack with a bend in it.
    while (t < 195000u){
        env_pass(node, bus, t, false);
        t += 1000u;
        TEST_ASSERT_EQUAL_INT16(0, node.value());
    }
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DELAY, node.stage());

    env_run(node, bus, t, 230000u, false);
    TEST_ASSERT_GREATER_THAN_INT16(0, node.value());
}

static void test_a_hold_keeps_the_peak_before_the_decay_starts() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 10;      // 50 ms
    c.params[EnvelopeNode::P_HOLD] = 20;        // 200 ms
    c.params[EnvelopeNode::P_DECAY] = 10;
    AdEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_pass(node, bus, t, true);
    t += 1000u;

    env_run(node, bus, t, 55000u, false);
    while (t < 245000u){
        env_pass(node, bus, t, false);
        t += 1000u;
        TEST_ASSERT_EQUAL(EnvelopeNode::ENV_HOLD, node.stage());
        TEST_ASSERT_EQUAL_INT16(CV_MAX, node.value());
    }
    env_run(node, bus, t, 260000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, node.stage());
}

static void test_an_adsr_waits_at_the_sustain_level_while_the_gate_is_up() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_ADSR, false);
    c.params[EnvelopeNode::P_ATTACK] = 10;          // 50 ms
    c.params[EnvelopeNode::P_DECAY] = 10;           // 50 ms
    c.params[AdsrEnvelope::P_SUSTAIN] = 50;         // half scale
    c.params[AdsrEnvelope::P_RELEASE] = 10;         // 50 ms
    AdsrEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_run(node, bus, t, 150000u, true);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_SUSTAIN, node.stage());
    TEST_ASSERT_INT16_WITHIN(40, CV_MAX / 2, node.value());

    // Held for ten seconds: still exactly where it was. The contour is as
    // long as the note, which is the whole of what a sustain is.
    const int16_t held = node.value();
    env_run(node, bus, t, 10000000u, true);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_SUSTAIN, node.stage());
    TEST_ASSERT_EQUAL_INT16(held, node.value());

    env_pass(node, bus, t, false);              // let go
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_RELEASE, node.stage());
    env_run(node, bus, t, 10060000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
    TEST_ASSERT_EQUAL_INT16(0, node.value());
}

static void test_the_release_leaves_from_wherever_the_contour_had_got_to() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_ADSR, false);
    c.params[EnvelopeNode::P_ATTACK] = 32;          // 512 ms: long enough to cut into
    c.params[AdsrEnvelope::P_RELEASE] = 20;         // 200 ms
    AdsrEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_run(node, bus, t, 150000u, true);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
    const int16_t caught = node.value();
    TEST_ASSERT_GREATER_THAN_INT16(200, caught);
    TEST_ASSERT_LESS_THAN_INT16(CV_MAX - 200, caught);

    // Let go part way up the attack: the release starts here and not at the
    // peak the contour never reached.
    env_pass(node, bus, t, false);
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_RELEASE, node.stage());
    TEST_ASSERT_INT16_WITHIN(60, caught, node.value());

    env_run(node, bus, t, 400000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
}

static void test_a_looping_adsr_never_reaches_the_sustain() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_ADSR, false);
    c.params[EnvelopeNode::P_ATTACK] = 10;          // 50 ms
    c.params[EnvelopeNode::P_DECAY] = 10;           // 50 ms
    c.params[AdsrEnvelope::P_SUSTAIN] = 50;
    c.params[AdsrEnvelope::P_RELEASE] = 10;
    c.params[AdsrEnvelope::P_LOOP] = EnvelopeNode::ENV_LOOP_HELD;
    AdsrEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;

    // Under a held gate the contour keeps starting over, so the level keeps
    // coming back to the top and back down rather than settling.
    uint8_t peaks = 0;
    bool climbing = true;
    int16_t previous = 0;
    while (t < 600000u){
        env_pass(node, bus, t, true);
        t += 1000u;
        TEST_ASSERT_NOT_EQUAL(EnvelopeNode::ENV_SUSTAIN, node.stage());
        if (climbing && node.value() < previous){ peaks++; climbing = false; }
        if (!climbing && node.value() > previous) climbing = true;
        previous = node.value();
    }
    // Five and a half cycles in half a second of 100 ms contours; the count
    // is loose because where the window lands is not the claim.
    TEST_ASSERT_GREATER_THAN_UINT8(3, peaks);

    // Letting go still releases it, from wherever round the loop it was.
    env_pass(node, bus, t, false);
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_RELEASE, node.stage());
    env_run(node, bus, t, t + 100000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
}

static void test_an_envelope_looping_always_runs_with_nothing_patched() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 10;          // 50 ms
    c.params[EnvelopeNode::P_DECAY] = 10;           // 50 ms
    c.params[AdEnvelope::P_LOOP] = EnvelopeNode::ENV_LOOP_FREE;
    AdEnvelope node(c);

    // It is a shape generator now: never idle, never triggered, and it visits
    // both ends of its travel.
    int16_t lowest = CV_MAX;
    int16_t highest = 0;
    uint32_t t = 0;
    while (t < 400000u){
        env_pass(node, bus, t, false);
        t += 1000u;
        TEST_ASSERT_NOT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
        if (node.value() < lowest) lowest = node.value();
        if (node.value() > highest) highest = node.value();
    }
    TEST_ASSERT_LESS_THAN_INT16(200, lowest);
    TEST_ASSERT_GREATER_THAN_INT16(CV_MAX - 200, highest);
}

static void test_a_synced_envelope_advances_on_the_clock_and_not_on_time() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_SYNC] = EnvelopeNode::ENV_CLOCK;
    c.params[EnvelopeNode::P_ATTACK_DIV] = DIV_16TH;
    c.params[EnvelopeNode::P_DECAY_DIV] = DIV_16TH;
    AdEnvelope node(c);

    const uint32_t sixteenth = division_subticks(DIV_16TH, FEEL_STRAIGHT);
    uint32_t count = 0;
    node.tick(bus, count);
    env_pass(node, bus, 0, false);
    env_pass(node, bus, 1000u, true);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
    // The stage is a note value, counted in subticks, and the delay and the
    // hold are off, so the contour is in the attack already.
    TEST_ASSERT_EQUAL_UINT32(sixteenth, node.stage_length());

    // A minute of wall clock with the clock stopped moves it nowhere: a
    // synced envelope is measured in the clock's own time.
    for (uint32_t t = 2000u; t < 60000000u; t += 1000000u) env_pass(node, bus, t, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
    TEST_ASSERT_EQUAL_INT16(0, node.value());

    count += sixteenth / 2u;
    node.tick(bus, count);
    env_pass(node, bus, 60000000u, false);
    TEST_ASSERT_INT16_WITHIN(60, CV_HALF, node.value());

    count += sixteenth;                              // through the top and into the decay
    node.tick(bus, count);
    env_pass(node, bus, 60001000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, node.stage());
}

static void test_a_triplet_stage_is_a_whole_number_of_subticks() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_SYNC] = EnvelopeNode::ENV_CLOCK;
    c.params[EnvelopeNode::P_FEEL] = FEEL_TRIPLET;
    c.params[EnvelopeNode::P_ATTACK_DIV] = DIV_EIGHTH;
    AdEnvelope node(c);
    node.tick(bus, 0);
    env_pass(node, bus, 0, false);
    env_pass(node, bus, 1000u, true);
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICKS_PER_QUARTER / 3u, node.stage_length());
}

static void test_a_stage_turned_off_takes_no_time_in_either_unit() {
    // Synced, "off" is a value on the note list and is worth nothing at all.
    TEST_ASSERT_EQUAL_UINT32(0, division_subticks(DIV_OFF, FEEL_STRAIGHT));
    TEST_ASSERT_EQUAL_UINT32(0, division_subticks(DIV_OFF, FEEL_TRIPLET));
    // Free-running, the same job is done by the bottom of the time control:
    // one pass of the graph, which is the shortest thing a wall clock can say.
    TEST_ASSERT_EQUAL_UINT32(500, param_env_time_us(EnvelopeNode::OFF_TIME));
    TEST_ASSERT_EQUAL_UINT32(32512500, param_env_time_us(255));
}

static void test_the_end_outlet_fires_a_trigger_when_the_contour_finishes() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 10;          // 50 ms
    c.params[EnvelopeNode::P_DECAY] = 10;           // 50 ms
    AdEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_pass(node, bus, t, true);
    t += 1000u;

    // Nothing while the contour is running ...
    while (t < 95000u){
        env_pass(node, bus, t, false);
        t += 1000u;
        TEST_ASSERT_FALSE(bus.gate_read(1));
    }
    // ... and a trigger once it has finished.
    bool fired = false;
    while (t < 120000u){
        env_pass(node, bus, t, false);
        t += 1000u;
        if (bus.gate_read(1)) fired = true;
    }
    TEST_ASSERT_TRUE(fired);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, node.stage());
}

static void test_retriggering_starts_from_zero_or_from_the_level_or_not_at_all() {
    for (uint8_t rule = EnvelopeNode::ENV_RETRIG_ZERO; rule <= EnvelopeNode::ENV_RETRIGGERS; rule++){
        BusManager bus;
        NodeConfig c = envelope_config(ALGO_AD, false);
        c.params[EnvelopeNode::P_ATTACK] = 10;      // 50 ms
        c.params[EnvelopeNode::P_DECAY] = 40;       // 800 ms, to catch half way down
        c.params[AdEnvelope::P_RETRIG] = rule;
        AdEnvelope node(c);

        uint32_t t = 0;
        env_pass(node, bus, t, false);
        t += 1000u;
        env_pass(node, bus, t, true);
        t += 1000u;
        env_run(node, bus, t, 450000u, false);
        TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, node.stage());
        const int16_t before = node.value();

        env_pass(node, bus, t, true);               // a second trigger, part way down
        t += 1000u;
        switch (rule){
            case EnvelopeNode::ENV_RETRIG_ZERO:
                // A hard retrigger: back to nothing and up again from there.
                TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
                TEST_ASSERT_LESS_THAN_INT16(200, node.value());
                break;
            case EnvelopeNode::ENV_RETRIG_LEVEL:
                // Up again from where it was, with no step in the middle.
                TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
                TEST_ASSERT_INT16_WITHIN(80, before, node.value());
                break;
            default:
                // Ignored: the contour it is already drawing finishes first.
                TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, node.stage());
                TEST_ASSERT_LESS_THAN_INT16(before, node.value());
                break;
        }
    }
}

static void test_level_scales_the_contour_and_invert_turns_it_over() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 10;
    c.params[EnvelopeNode::P_HOLD] = 40;            // sit at the peak to read it
    c.params[AdEnvelope::P_LEVEL] = 50;
    AdEnvelope quiet(c);
    c.params[AdEnvelope::P_INVERT] = 1;
    AdEnvelope upside_down(c);

    BusManager other;
    uint32_t t = 0;
    env_pass(quiet, bus, t, false);
    env_pass(upside_down, other, t, false);
    t += 1000u;
    env_pass(quiet, bus, t, true);
    env_pass(upside_down, other, t, true);
    t += 1000u;
    while (t < 200000u){
        env_pass(quiet, bus, t, false);
        env_pass(upside_down, other, t, false);
        t += 1000u;
    }
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_HOLD, quiet.stage());
    // Half level, and the same contour the other side of zero.
    TEST_ASSERT_INT16_WITHIN(2, CV_MAX / 2, quiet.value());
    TEST_ASSERT_EQUAL_INT16(-quiet.value(), upside_down.value());
}

static void test_a_curve_bends_the_travel_without_moving_its_ends() {
    // The ends are the ends whatever the curve: a stage that started
    // somewhere else or stopped short would be a curve that changed the
    // contour's shape rather than how it is walked.
    for (uint8_t amount = PARAM_CENTRE - 100; amount <= PARAM_CENTRE + 100; amount++){
        TEST_ASSERT_EQUAL_UINT32(0, env_curve(amount, 0));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)CV_FULL, env_curve(amount, CV_FULL));
    }
    // Straight through the middle is the middle.
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CV_HALF, env_curve(PARAM_CENTRE, CV_HALF));
    // Fully positive creeps then runs: a quarter of the way along at half
    // time. Fully negative is the mirror of it.
    TEST_ASSERT_UINT32_WITHIN(16, (uint32_t)CV_FULL / 4u, env_curve(PARAM_CENTRE + 100, CV_HALF));
    TEST_ASSERT_UINT32_WITHIN(16, (uint32_t)(3 * CV_FULL) / 4u, env_curve(PARAM_CENTRE - 100, CV_HALF));
    // And every curve is still a walk in one direction.
    for (uint32_t frac = 1; frac <= (uint32_t)CV_FULL; frac++){
        TEST_ASSERT_TRUE(env_curve(PARAM_CENTRE + 100, frac) >= env_curve(PARAM_CENTRE + 100, frac - 1));
        TEST_ASSERT_TRUE(env_curve(PARAM_CENTRE - 100, frac) >= env_curve(PARAM_CENTRE - 100, frac - 1));
    }
}

static void test_a_stage_lengthened_under_a_finger_does_not_jump() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, false);
    c.params[EnvelopeNode::P_ATTACK] = 20;          // 200 ms
    AdEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_pass(node, bus, t, true);
    t += 1000u;
    env_run(node, bus, t, 100000u, false);
    const int16_t half_way = node.value();
    TEST_ASSERT_INT16_WITHIN(150, CV_HALF, half_way);

    // Four times as long, set under a finger. The contour keeps the fraction
    // of the stage it had covered rather than snapping back to the bottom.
    TEST_ASSERT_TRUE(node.set_param(EnvelopeNode::P_ATTACK, 40));
    env_pass(node, bus, t, false);
    t += 1000u;
    TEST_ASSERT_INT16_WITHIN(60, half_way, node.value());
    // And it is still climbing, at the new rate.
    env_run(node, bus, t, 150000u, false);
    TEST_ASSERT_GREATER_THAN_INT16(half_way, node.value());
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
}

static void test_an_ad_reset_edge_cuts_the_contour() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_AD, true);
    c.params[EnvelopeNode::P_ATTACK] = 10;
    c.params[EnvelopeNode::P_DECAY] = 40;           // 800 ms
    c.params[AdEnvelope::P_LOOP] = EnvelopeNode::ENV_LOOP_FREE;
    AdEnvelope node(c);

    uint32_t t = 0;
    env_run(node, bus, t, 300000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, node.stage());
    TEST_ASSERT_GREATER_THAN_INT16(CV_HALF, node.value());

    // A reset cuts it to nothing, which is the only handle there is on a
    // loop that never asked for a gate. An envelope told to loop always then
    // starts the next contour at once - it was told to run, and a reset says
    // where from, not whether.
    env_pass(node, bus, t, false, true);
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
    TEST_ASSERT_LESS_THAN_INT16(200, node.value());

    // One that is not looping stops there and stays stopped.
    BusManager once_bus;
    c.params[AdEnvelope::P_LOOP] = EnvelopeNode::ENV_LOOP_OFF;
    AdEnvelope one_shot(c);
    uint32_t u = 0;
    env_pass(one_shot, once_bus, u, false);
    u += 1000u;
    env_pass(one_shot, once_bus, u, true);
    u += 1000u;
    env_run(one_shot, once_bus, u, 300000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_DECAY, one_shot.stage());
    env_pass(one_shot, once_bus, u, false, true);
    u += 1000u;
    env_run(one_shot, once_bus, u, 400000u, false);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_IDLE, one_shot.stage());
    TEST_ASSERT_EQUAL_INT16(0, one_shot.value());
}

static void test_an_adsr_retrig_inlet_fires_under_a_held_gate() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_ADSR, true);
    c.params[EnvelopeNode::P_ATTACK] = 10;
    c.params[EnvelopeNode::P_DECAY] = 10;
    c.params[AdsrEnvelope::P_SUSTAIN] = 40;
    AdsrEnvelope node(c);

    uint32_t t = 0;
    env_pass(node, bus, t, false);
    t += 1000u;
    env_run(node, bus, t, 150000u, true);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_SUSTAIN, node.stage());

    // A second note under the first: the contour starts again without the
    // gate ever having fallen.
    env_pass(node, bus, t, true, true);
    t += 1000u;
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_ATTACK, node.stage());
    env_run(node, bus, t, 250000u, true);
    TEST_ASSERT_EQUAL(EnvelopeNode::ENV_SUSTAIN, node.stage());
}

static void test_an_envelope_never_allocates() {
    BusManager bus;
    NodeConfig c = envelope_config(ALGO_ADSR, true);
    AdsrEnvelope node(c);
    const size_t before = g_allocations;
    uint32_t t = 0;
    while (t < 2000000u){
        env_pass(node, bus, t, (t / 100000u) % 2u == 0u);
        node.tick(bus, t / 1000u);
        t += 1000u;
    }
    TEST_ASSERT_TRUE(node.set_param(EnvelopeNode::P_ATTACK, 60));
    TEST_ASSERT_TRUE(node.set_param(AdsrEnvelope::P_RELEASE_DIV, DIV_BAR));
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

    RUN_TEST(test_a_step_reads_the_centre_of_its_slice_of_the_shape);
    RUN_TEST(test_a_bipolar_shape_is_centred_on_zero);
    RUN_TEST(test_the_level_holds_between_triggers);
    RUN_TEST(test_direction_walks_the_steps_backwards_and_bounces);
    RUN_TEST(test_reset_sends_the_next_trigger_to_the_first_step);
    RUN_TEST(test_steps_cut_the_shape_finer_and_the_edit_is_heard_at_once);
    RUN_TEST(test_depth_and_offset_move_the_level_already_held);
    RUN_TEST(test_random_step_draws_a_level_on_every_trigger);
    RUN_TEST(test_random_glide_steps_between_two_levels_over_one_period);

    RUN_TEST(test_an_ad_rises_to_the_peak_and_falls_back_to_nothing);
    RUN_TEST(test_the_length_of_the_trigger_means_nothing_to_an_ad);
    RUN_TEST(test_a_pre_delay_keeps_the_contour_at_nothing_first);
    RUN_TEST(test_a_hold_keeps_the_peak_before_the_decay_starts);
    RUN_TEST(test_an_adsr_waits_at_the_sustain_level_while_the_gate_is_up);
    RUN_TEST(test_the_release_leaves_from_wherever_the_contour_had_got_to);
    RUN_TEST(test_a_looping_adsr_never_reaches_the_sustain);
    RUN_TEST(test_an_envelope_looping_always_runs_with_nothing_patched);
    RUN_TEST(test_a_synced_envelope_advances_on_the_clock_and_not_on_time);
    RUN_TEST(test_a_triplet_stage_is_a_whole_number_of_subticks);
    RUN_TEST(test_a_stage_turned_off_takes_no_time_in_either_unit);
    RUN_TEST(test_the_end_outlet_fires_a_trigger_when_the_contour_finishes);
    RUN_TEST(test_retriggering_starts_from_zero_or_from_the_level_or_not_at_all);
    RUN_TEST(test_level_scales_the_contour_and_invert_turns_it_over);
    RUN_TEST(test_a_curve_bends_the_travel_without_moving_its_ends);
    RUN_TEST(test_a_stage_lengthened_under_a_finger_does_not_jump);
    RUN_TEST(test_an_ad_reset_edge_cuts_the_contour);
    RUN_TEST(test_an_adsr_retrig_inlet_fires_under_a_held_gate);
    RUN_TEST(test_an_envelope_never_allocates);

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
    RUN_TEST(test_a_route_says_what_it_is_doing);
    RUN_TEST(test_a_route_says_why_it_is_doing_nothing);
    RUN_TEST(test_two_offset_routes_on_one_parameter_sum);
    RUN_TEST(test_a_route_to_a_parameter_that_does_not_exist_is_refused);
    RUN_TEST(test_a_route_cannot_press_the_transport);
    RUN_TEST(test_a_route_can_drive_the_tempo);
    RUN_TEST(test_editing_a_route_forgets_the_centre_it_was_holding);
    RUN_TEST(test_routes_round_trip_through_the_codec);
    RUN_TEST(test_a_patch_from_another_format_version_is_refused);
    RUN_TEST(test_a_running_modulator_does_not_wear_out_the_eeprom);
    RUN_TEST(test_the_patch_image_keeps_the_set_point_not_the_modulation);
    RUN_TEST(test_quantising_keeps_a_bipolar_signal_bipolar);
    RUN_TEST(test_a_unipolar_signal_read_as_bipolar_keeps_its_whole_travel);
    RUN_TEST(test_the_modulation_path_never_allocates);
    return UNITY_END();
}
