#include <unity.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "node/patch.h"
#include "node/registry.h"
#include "control/macros.h"
#include "control/cc_mapper.h"
#include "control/control_sum.h"
#include "control/mod_matrix.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "hal/midi_types.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

static constexpr uint8_t KEYBOARD = mmMIDI_USB_0;
// ClockDiv's "amount": 1..255, so there is room either side of a set point.
static constexpr uint16_t AMOUNT = 1;
static constexpr uint16_t PHASE = 2;

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
            patches(master, store, leds), macros(), cc(patches, master, macros),
            sum(cc), mod(patches, cc, sum) {}

    // One main-loop turn, in main.cpp's order.
    void turn(uint32_t now_us){
        cc.apply(now_us);
        sum.begin();
        mod.apply(master.buses(), now_us);
        macros.expand(patches.active(), sum);
        sum.commit(now_us);
        master.pass(now_us);
    }

    uint8_t param(uint8_t node, uint16_t index){
        uint8_t v = 0;
        TEST_ASSERT_TRUE(master.get_node_param(node, index, v));
        return v;
    }
};

// One ClockDiv, with two parameters worth moving and a set point on each.
static Patch macro_patch(){
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_buses[0] = one_bus(0);
    p.nodes[0].params[AMOUNT] = 100;
    p.nodes[0].params[PHASE] = 100;
    p.n_nodes = 1;
    return p;
}

static void name_macro(Patch& p, uint8_t index, const char* name){
    memset(p.macros[index].name, 0, MACRO_NAME_BYTES);
    for (uint8_t i = 0; i < MACRO_NAME_BYTES && name[i] != 0; i++){
        p.macros[index].name[i] = name[i];
    }
}

static MacroDest dest_to(uint8_t macro, uint16_t param, uint8_t lo, uint8_t hi, int16_t depth){
    MacroDest d = unused_dest();
    d.macro = macro;
    d.target_kind = CC_TARGET_NODE;
    d.target_index = 0;
    d.param = param;
    d.src_lo = lo;
    d.src_hi = hi;
    d.depth = depth;
    return d;
}

// The rule that makes a macro safe to load: a macro that nobody has touched
// has no position, so it must not assert one. Without this, opening a patch
// would slam every macro'd parameter to whatever position zero happens to
// mean and destroy the sound that was just loaded.
static void test_an_untouched_macro_leaves_its_destinations_alone(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "open up");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 100);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    for (uint32_t t = 0; t < 10000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));
    TEST_ASSERT_FALSE(rig.macros.engaged(0));
}

static void test_moving_a_macro_engages_it_and_it_then_holds(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "open up");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 100);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    TEST_ASSERT_TRUE(rig.macros.set(0, 255));
    rig.turn(1000u);
    TEST_ASSERT_TRUE(rig.macros.engaged(0));
    TEST_ASSERT_EQUAL_UINT8(200, rig.param(0, AMOUNT));

    // Held, not written once: it is still there several passes later.
    for (uint32_t t = 2000u; t < 20000u; t += 1000u) rig.turn(t);
    TEST_ASSERT_EQUAL_UINT8(200, rig.param(0, AMOUNT));
}

// Below its window a destination asks for nothing; above it, it holds the
// whole of its depth rather than falling back. Sweeping a macro up builds.
static void test_a_window_holds_above_its_top_and_is_silent_below_its_floor(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "stage");
    p.macro_dest[0] = dest_to(0, AMOUNT, 64, 128, 50);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    rig.macros.set(0, 0);    rig.turn(1000u);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));   // below: nothing
    rig.macros.set(0, 64);   rig.turn(2000u);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));   // at the floor: nothing
    rig.macros.set(0, 96);   rig.turn(3000u);
    TEST_ASSERT_UINT8_WITHIN(2, 125, rig.param(0, AMOUNT)); // half way
    rig.macros.set(0, 128);  rig.turn(4000u);
    TEST_ASSERT_EQUAL_UINT8(150, rig.param(0, AMOUNT));   // the top of the window
    rig.macros.set(0, 255);  rig.turn(5000u);
    TEST_ASSERT_EQUAL_UINT8(150, rig.param(0, AMOUNT));   // past it: held, not released
}

// Sweeping back down must return the parameter to where the patch set it.
// This is why a zero contribution is still offered to the sum: a target
// nobody asks for keeps whatever was last written to it.
static void test_sweeping_a_macro_back_down_returns_the_set_point(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "stage");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 80);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    rig.macros.set(0, 255); rig.turn(1000u);
    TEST_ASSERT_EQUAL_UINT8(180, rig.param(0, AMOUNT));
    rig.macros.set(0, 0);   rig.turn(2000u);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));
}

// The shape hold cannot make on its own, built out of hold: two adjacent
// windows with opposite depths rise and then fall back. This is the reason
// holding is the primitive and releasing is not.
static void test_two_opposite_windows_make_a_rise_and_a_fall(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "bell");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 128, 50);
    p.macro_dest[1] = dest_to(0, AMOUNT, 128, 255, -50);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    rig.macros.set(0, 0);   rig.turn(1000u);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));
    rig.macros.set(0, 128); rig.turn(2000u);
    TEST_ASSERT_EQUAL_UINT8(150, rig.param(0, AMOUNT));   // the peak
    rig.macros.set(0, 255); rig.turn(3000u);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));   // home again
}

// One control, two parameters, each over its own half of the travel - the
// thing macros exist for.
static void test_one_macro_stages_two_parameters(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "sweep");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 128, 60);
    p.macro_dest[1] = dest_to(0, PHASE, 128, 255, 60);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    rig.macros.set(0, 128); rig.turn(1000u);
    TEST_ASSERT_EQUAL_UINT8(160, rig.param(0, AMOUNT));   // the first half moved
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, PHASE));    // the second has not started

    rig.macros.set(0, 255); rig.turn(2000u);
    TEST_ASSERT_EQUAL_UINT8(160, rig.param(0, AMOUNT));   // still open
    TEST_ASSERT_EQUAL_UINT8(160, rig.param(0, PHASE));    // and now the second
}

// The sum is clipped once, against the target's range - not once per
// contribution, which would quietly change what a sum means near the ends.
static void test_contributions_are_clipped_once_after_the_sum(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "hard");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 120);
    p.macro_dest[1] = dest_to(0, AMOUNT, 0, 255, 120);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    rig.macros.set(0, 255); rig.turn(1000u);
    // 100 + 120 + 120 = 340, and the parameter's ceiling is 255.
    TEST_ASSERT_EQUAL_UINT8(255, rig.param(0, AMOUNT));
}

// A macro is a target like any other, which is the whole reason it is not a
// source: a knob reaches one through the same table, with the same takeover.
static void test_a_cc_moves_a_macro_through_the_binding_table(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "knob");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 100);
    p.cc_map[0] = unused_mapping();
    p.cc_map[0].source_mask = KEYBOARD;
    p.cc_map[0].channel = 1;
    p.cc_map[0].cc = 20;
    p.cc_map[0].target_kind = CC_TARGET_MACRO;
    p.cc_map[0].target_index = 0;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    TEST_ASSERT_TRUE(rig.cc.observe(KEYBOARD, 1, 20, 127, 0));
    rig.turn(1000u);
    TEST_ASSERT_TRUE(rig.macros.engaged(0));
    TEST_ASSERT_EQUAL_UINT8(200, rig.param(0, AMOUNT));
}

// A macro's position is not patch state. Recalling a patch must not
// reproduce where a macro was left, because a macro is a hand on the
// instrument and not part of it.
static void test_a_patch_load_forgets_where_a_macro_was(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "knob");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 100);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    rig.macros.set(0, 255);
    rig.turn(1000u);
    TEST_ASSERT_EQUAL_UINT8(200, rig.param(0, AMOUNT));

    // What every load path in src does after applying a whole patch.
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 2000u));
    rig.cc.reset();
    rig.turn(3000u);
    TEST_ASSERT_FALSE(rig.macros.engaged(0));
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));
}

static void test_a_macro_may_not_target_a_macro(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "a");
    name_macro(p, 1, "b");
    MacroDest d = unused_dest();
    d.macro = 0;
    d.target_kind = CC_TARGET_MACRO;
    d.target_index = 1;
    p.macro_dest[0] = d;
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    TEST_ASSERT_EQUAL(LOAD_MACRO_DEST_INVALID, rig.master.last_error());
}

static void test_one_macro_may_not_eat_the_shared_pool(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "greedy");
    for (uint8_t i = 0; i <= N_MACRO_DEST_PER_MACRO; i++){
        p.macro_dest[i] = dest_to(0, AMOUNT, 0, 255, 1);
    }
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    TEST_ASSERT_EQUAL(LOAD_MACRO_DEST_INVALID, rig.master.last_error());
}

static void test_a_destination_to_a_parameter_that_does_not_exist_is_refused(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "stale");
    p.macro_dest[0] = dest_to(0, 900, 0, 255, 10);
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    TEST_ASSERT_EQUAL(LOAD_MACRO_DEST_INVALID, rig.master.last_error());
}

// A window with no width is a step, not a division by zero - and a useful
// destination in its own right: "from here on".
static void test_a_window_with_no_width_is_a_step(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "switch");
    p.macro_dest[0] = dest_to(0, AMOUNT, 200, 200, 40);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    rig.macros.set(0, 199); rig.turn(1000u);
    TEST_ASSERT_EQUAL_UINT8(100, rig.param(0, AMOUNT));
    rig.macros.set(0, 200); rig.turn(2000u);
    TEST_ASSERT_EQUAL_UINT8(140, rig.param(0, AMOUNT));
}

// The macro layer is part of the image, so it survives a save. The position
// is not, which is the point of the test above.
static void test_macros_round_trip_through_the_codec(){
    Patch p = macro_patch();
    name_macro(p, 0, "open up");
    name_macro(p, 3, "wreck");
    p.macro_dest[0] = dest_to(0, AMOUNT, 10, 200, 55);
    p.macro_dest[5] = dest_to(3, PHASE, 128, 255, -70);

    static uint8_t image[PATCH_SLOT_BYTES];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(p, default_globals(), image, sizeof(image), written));

    Patch back = empty_patch();
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(image, written, back, g));

    TEST_ASSERT_EQUAL_INT(0, memcmp("open up", back.macros[0].name, 7));
    TEST_ASSERT_EQUAL_INT(0, memcmp("wreck", back.macros[3].name, 5));
    TEST_ASSERT_FALSE(macro_used(back.macros[1]));

    TEST_ASSERT_EQUAL_UINT8(0, back.macro_dest[0].macro);
    TEST_ASSERT_EQUAL_UINT16(AMOUNT, back.macro_dest[0].param);
    TEST_ASSERT_EQUAL_UINT8(10, back.macro_dest[0].src_lo);
    TEST_ASSERT_EQUAL_UINT8(200, back.macro_dest[0].src_hi);
    TEST_ASSERT_EQUAL_INT16(55, back.macro_dest[0].depth);

    // Signed depth survives the round trip: a macro that closes something
    // while opening something else is the reason depth is signed at all.
    TEST_ASSERT_EQUAL_UINT8(3, back.macro_dest[5].macro);
    TEST_ASSERT_EQUAL_INT16(-70, back.macro_dest[5].depth);
    TEST_ASSERT_EQUAL_UINT8(MACRO_NONE, back.macro_dest[1].macro);
}

// A patch with no macros must cost nothing on the wire, exactly as a patch
// with no mappings and no routes does.
static void test_a_patch_with_no_macros_costs_two_bytes(){
    static uint8_t with[PATCH_SLOT_BYTES];
    static uint8_t without[PATCH_SLOT_BYTES];
    size_t a = 0, b = 0;

    Patch bare = macro_patch();
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(bare, default_globals(), without, sizeof(without), b));

    Patch one = macro_patch();
    name_macro(one, 0, "m");
    one.macro_dest[0] = dest_to(0, AMOUNT, 0, 255, 10);
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(one, default_globals(), with, sizeof(with), a));

    TEST_ASSERT_GREATER_THAN_size_t(b, a);
}

// The macro layer is fixed-size state in the Patch and two small arrays; a
// pass that sweeps a macro across several destinations must not reach the
// heap, exactly as no other pass does.
static void test_a_macro_pass_allocates_nothing(){
    Rig rig;
    Patch p = macro_patch();
    name_macro(p, 0, "sweep");
    p.macro_dest[0] = dest_to(0, AMOUNT, 0, 128, 60);
    p.macro_dest[1] = dest_to(0, PHASE, 128, 255, 60);
    p.macro_dest[2] = dest_to(0, AMOUNT, 128, 255, -20);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));

    const size_t before = g_allocations;
    for (uint32_t t = 0; t < 64000u; t += 1000u){
        rig.macros.set(0, (uint8_t)(t / 250u));
        rig.turn(t);
    }
    TEST_ASSERT_EQUAL(before, g_allocations);
}

int main(int, char**){
    UNITY_BEGIN();
    RUN_TEST(test_an_untouched_macro_leaves_its_destinations_alone);
    RUN_TEST(test_moving_a_macro_engages_it_and_it_then_holds);
    RUN_TEST(test_a_window_holds_above_its_top_and_is_silent_below_its_floor);
    RUN_TEST(test_sweeping_a_macro_back_down_returns_the_set_point);
    RUN_TEST(test_two_opposite_windows_make_a_rise_and_a_fall);
    RUN_TEST(test_one_macro_stages_two_parameters);
    RUN_TEST(test_contributions_are_clipped_once_after_the_sum);
    RUN_TEST(test_a_cc_moves_a_macro_through_the_binding_table);
    RUN_TEST(test_a_patch_load_forgets_where_a_macro_was);
    RUN_TEST(test_a_macro_may_not_target_a_macro);
    RUN_TEST(test_one_macro_may_not_eat_the_shared_pool);
    RUN_TEST(test_a_destination_to_a_parameter_that_does_not_exist_is_refused);
    RUN_TEST(test_a_window_with_no_width_is_a_step);
    RUN_TEST(test_macros_round_trip_through_the_codec);
    RUN_TEST(test_a_patch_with_no_macros_costs_two_bytes);
    RUN_TEST(test_a_macro_pass_allocates_nothing);
    return UNITY_END();
}
