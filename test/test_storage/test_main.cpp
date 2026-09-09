#include <unity.h>
#include <stdlib.h>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/fake_console_io.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "patch/patch_codec.h"
#include "patch/patch_store.h"
#include "patch/patch_manager.h"
#include "patch/default_patch.h"
#include "console/console.h"
#include "led/status_leds.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "algorithm/sequencer/note_sequencer.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// Patch storage, the status LEDs and the console (#7).

// One rig, because everything here is about how the pieces fit together.
struct Rig {
    FakeGpio gpio;
    RecordingMidiOut midi;
    FakeEeprom eeprom;
    FakeLeds led_driver;
    FakeConsoleIo io;
    MixedModeMaster master;
    StatusLeds leds;
    PatchStore store;
    PatchManager patches;
    Console console;

    Rig() : gpio(), midi(), eeprom(), led_driver(), io(),
            master(gpio, midi), leds(led_driver), store(eeprom),
            patches(master, store, leds),
            console(io, patches, master, store, leds) {}
};

static Patch three_node_patch() {
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, 3};
    p.midi_in[0] = MidiInConfig{0x11, 2, 1};
    p.midi_out[2] = MidiOutConfig{0x30, 5, 1};

    p.nodes[0] = node_config(ALGO_LOGIC_NOT);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 1;

    p.nodes[1] = node_config(ALGO_EUCLID_SEQ);
    p.nodes[1].in_bus[0] = 1;
    p.nodes[1].out_bus[0] = 3;
    p.nodes[1].params[0] = 16;      // length
    p.nodes[1].params[3] = 5;       // pulses
    p.nodes[1].params[4] = 2;       // rotation
    p.nodes[1].params[8 + 7] = 40;  // step 7's probability

    p.nodes[2] = node_config(ALGO_TRANSPOSE);
    p.nodes[2].in_bus[0] = 1;
    p.nodes[2].out_bus[0] = 1;
    p.nodes[2].params[0] = (uint8_t)(int8_t)-5;
    p.n_nodes = 3;
    return p;
}

static bool patches_equal(const Patch& a, const Patch& b) {
    if (a.n_nodes != b.n_nodes) return false;
    for (uint8_t i = 0; i < GPIO_N; i++) {
        if (a.gate_ports[i].direction != b.gate_ports[i].direction) return false;
        if (a.gate_ports[i].bus != b.gate_ports[i].bus) return false;
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++) {
        if (a.midi_in[i].source_mask != b.midi_in[i].source_mask) return false;
        if (a.midi_in[i].channel != b.midi_in[i].channel) return false;
        if (a.midi_in[i].bus != b.midi_in[i].bus) return false;
    }
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++) {
        if (a.midi_out[i].target_mask != b.midi_out[i].target_mask) return false;
        if (a.midi_out[i].channel != b.midi_out[i].channel) return false;
        if (a.midi_out[i].bus != b.midi_out[i].bus) return false;
    }
    for (uint8_t n = 0; n < a.n_nodes; n++) {
        if (a.nodes[n].algorithm_id != b.nodes[n].algorithm_id) return false;
        for (uint8_t i = 0; i < MAX_IN; i++) if (a.nodes[n].in_bus[i] != b.nodes[n].in_bus[i]) return false;
        for (uint8_t i = 0; i < MAX_OUT; i++) if (a.nodes[n].out_bus[i] != b.nodes[n].out_bus[i]) return false;
        for (uint16_t i = 0; i < N_PARAM; i++) if (a.nodes[n].params[i] != b.nodes[n].params[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The codec: encode -> decode is byte-identical, including the parameter
// bytes past the last non-zero one, which are not transmitted at all.
// ---------------------------------------------------------------------------
static void test_patch_round_trips_through_the_codec() {
    static uint8_t buffer[PATCH_SLOT_BYTES];
    const Patch original = three_node_patch();
    GlobalSettings g = default_globals();
    g.bpm = 143;
    g.cv_ppqn = 2;
    g.pc_enabled = 1;
    g.pc_channel = 7;

    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(original, g, buffer, sizeof buffer, written));
    TEST_ASSERT_TRUE(written > 0);

    Patch decoded = empty_patch();
    GlobalSettings decoded_globals = {};
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(buffer, written, decoded, decoded_globals));
    TEST_ASSERT_TRUE(patches_equal(original, decoded));
    TEST_ASSERT_EQUAL(143, decoded_globals.bpm);
    TEST_ASSERT_EQUAL(2, decoded_globals.cv_ppqn);
    TEST_ASSERT_EQUAL(1, decoded_globals.pc_enabled);
    TEST_ASSERT_EQUAL(7, decoded_globals.pc_channel);
}

// The whole point of trimming: a patch of ordinary nodes is a couple of
// hundred bytes, not the 11 KB sizeof(Patch) would be, which is what makes a
// preset slot fit the Teensy's ~4 KB of EEPROM at all.
static void test_trailing_zero_parameters_are_not_stored() {
    static uint8_t buffer[PATCH_SLOT_BYTES];
    GlobalSettings g = default_globals();
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(three_node_patch(), g, buffer, sizeof buffer, written));
    TEST_ASSERT_TRUE_MESSAGE(written < 256, "a three-node patch should be well under a slot");
    TEST_ASSERT_TRUE(sizeof(Patch) > 8000);            // what storing it raw would cost
}

static void test_a_corrupted_image_is_refused() {
    static uint8_t buffer[PATCH_SLOT_BYTES];
    GlobalSettings g = default_globals();
    size_t written = 0;
    patch_codec::encode(three_node_patch(), g, buffer, sizeof buffer, written);

    Patch decoded = empty_patch();
    GlobalSettings decoded_globals = {};

    buffer[20] ^= 0x40;                                 // one bit, in the payload
    TEST_ASSERT_EQUAL(CODEC_BAD_CRC, patch_codec::decode(buffer, written, decoded, decoded_globals));
    buffer[20] ^= 0x40;

    buffer[4] = PATCH_FORMAT_VERSION + 1;               // a version we do not know
    TEST_ASSERT_EQUAL(CODEC_BAD_VERSION, patch_codec::decode(buffer, written, decoded, decoded_globals));
    buffer[4] = PATCH_FORMAT_VERSION;

    buffer[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(CODEC_BAD_MAGIC, patch_codec::decode(buffer, written, decoded, decoded_globals));
    buffer[0] ^= 0xFF;

    TEST_ASSERT_EQUAL(CODEC_TRUNCATED, patch_codec::decode(buffer, written - 4, decoded, decoded_globals));
}

static void test_encoding_into_too_small_a_buffer_reports_rather_than_overruns() {
    uint8_t tiny[16];
    GlobalSettings g = default_globals();
    size_t written = 999;
    TEST_ASSERT_EQUAL(CODEC_NO_ROOM, patch_codec::encode(three_node_patch(), g, tiny, sizeof tiny, written));
    TEST_ASSERT_EQUAL(0, written);
}

// ---------------------------------------------------------------------------
// The store.
// ---------------------------------------------------------------------------
static void test_a_fresh_eeprom_reads_as_empty_not_corrupt() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    Patch p;
    GlobalSettings g;
    for (uint8_t s = 0; s < PATCH_SLOTS; s++) {
        TEST_ASSERT_EQUAL(STORE_EMPTY, store.load(s, p, g));
        TEST_ASSERT_FALSE(store.occupied(s));
    }
}

static void test_save_and_load_a_slot() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    const Patch original = three_node_patch();
    GlobalSettings g = default_globals();
    g.bpm = 96;

    TEST_ASSERT_EQUAL(STORE_OK, store.save(2, original, g));
    TEST_ASSERT_TRUE(store.occupied(2));
    TEST_ASSERT_TRUE(store.used(2) > 0);

    Patch back = empty_patch();
    GlobalSettings back_globals = {};
    TEST_ASSERT_EQUAL(STORE_OK, store.load(2, back, back_globals));
    TEST_ASSERT_TRUE(patches_equal(original, back));
    TEST_ASSERT_EQUAL(96, back_globals.bpm);

    // Slots are independent: writing 2 did not disturb 0.
    TEST_ASSERT_FALSE(store.occupied(0));
    TEST_ASSERT_EQUAL(STORE_NO_SUCH_SLOT, store.save(PATCH_SLOTS, original, g));
}

static void test_a_corrupt_slot_does_not_take_the_others_with_it() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    const Patch p = three_node_patch();
    GlobalSettings g = default_globals();
    store.save(0, p, g);
    store.save(1, p, g);

    eeprom.poke(PATCH_SLOT_BYTES + 30, (uint8_t)(eeprom.read(PATCH_SLOT_BYTES + 30) ^ 0x80));

    Patch back;
    GlobalSettings back_globals;
    TEST_ASSERT_EQUAL(STORE_CORRUPT, store.load(1, back, back_globals));
    TEST_ASSERT_EQUAL(STORE_OK, store.load(0, back, back_globals));
    TEST_ASSERT_TRUE(patches_equal(p, back));
}

static void test_erasing_a_slot_makes_it_empty_again() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    GlobalSettings g = default_globals();
    store.save(1, three_node_patch(), g);
    TEST_ASSERT_TRUE(store.occupied(1));
    store.erase(1);
    TEST_ASSERT_FALSE(store.occupied(1));
    Patch p; GlobalSettings back;
    TEST_ASSERT_EQUAL(STORE_EMPTY, store.load(1, p, back));
}

// Endurance: re-saving an identical patch changes no cell at all.
static void test_resaving_an_unchanged_patch_costs_no_writes() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    const Patch p = three_node_patch();
    GlobalSettings g = default_globals();
    store.save(0, p, g);
    const uint32_t after_first = eeprom.changed;
    TEST_ASSERT_TRUE(after_first > 0);
    store.save(0, p, g);
    TEST_ASSERT_EQUAL(after_first, eeprom.changed);
}

// The autosave collapses a burst of edits into one write, and writes even if
// the edits never stop.
static void test_autosave_is_debounced() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    const Patch p = three_node_patch();
    GlobalSettings g = default_globals();

    uint32_t now = 1000;
    store.mark_dirty(now);
    TEST_ASSERT_TRUE(store.dirty());
    for (uint32_t t = 0; t < PatchStore::AUTOSAVE_SETTLE_US; t += 50000) {
        store.mark_dirty(now + t);                              // edits keep arriving
        TEST_ASSERT_FALSE(store.service(now + t, p, g));        // nothing written yet
    }
    TEST_ASSERT_TRUE(store.service(now + PatchStore::AUTOSAVE_SETTLE_US, p, g));
    TEST_ASSERT_FALSE(store.dirty());
    TEST_ASSERT_EQUAL(1, store.writes());
    TEST_ASSERT_TRUE(store.occupied(0));
}

// ---------------------------------------------------------------------------
// Boot: the acceptance criteria from #7.
// ---------------------------------------------------------------------------
static void test_an_empty_module_boots_into_a_patch_that_does_something() {
    Rig rig;
    rig.patches.boot(0);

    TEST_ASSERT_TRUE(rig.patches.running_defaults());
    TEST_ASSERT_TRUE(rig.master.node_count() > 0);
    // MIDI thru, both ways, and the control cable is not in either mask: a
    // patch cannot reroute the protocol's own port.
    TEST_ASSERT_TRUE(rig.patches.active().midi_in[0].source_mask != 0);
    TEST_ASSERT_TRUE(rig.patches.active().midi_out[0].target_mask != 0);
    TEST_ASSERT_EQUAL(0, rig.patches.active().midi_out[0].target_mask & MIDI_CONTROL_PORT);
    TEST_ASSERT_EQUAL(0, rig.patches.active().midi_in[0].source_mask & MIDI_CONTROL_PORT);
    // And a pulse on jack 1, so the module is visibly alive.
    TEST_ASSERT_EQUAL(GATE_PORT_OUT, rig.patches.active().gate_ports[0].direction);
}

// An empty store is a new module, not a fault: red stays off.
static void test_an_empty_store_does_not_light_red() {
    Rig rig;
    rig.patches.boot(0);
    rig.leds.service(10000000);           // past the boot identify pattern
    TEST_ASSERT_EQUAL(0, rig.led_driver.levels[LED_RED]);
}

// A corrupt store is a fault: red is solid, and stays solid, until a valid
// patch arrives.
static void test_a_corrupt_store_boots_the_default_and_lights_red() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.store.save(0, three_node_patch(), g);
    rig.eeprom.poke(40, (uint8_t)(rig.eeprom.read(40) ^ 0x20));

    rig.patches.boot(0);
    TEST_ASSERT_TRUE(rig.patches.running_defaults());
    rig.leds.service(10000000);
    TEST_ASSERT_EQUAL(StatusLeds::BRIGHT, rig.led_driver.levels[LED_RED]);

    // Loading something valid clears it.
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(three_node_patch(), g, 11000000));
    rig.leds.service(11000000);
    TEST_ASSERT_EQUAL(0, rig.led_driver.levels[LED_RED]);
}

// Saved, "power-cycled", reloaded: the same graph, byte for byte.
static void test_a_saved_patch_survives_a_power_cycle() {
    FakeEeprom eeprom;
    Patch saved;
    {
        FakeGpio gpio; RecordingMidiOut midi; FakeLeds ld;
        MixedModeMaster master(gpio, midi);
        StatusLeds leds(ld);
        PatchStore store(eeprom);
        PatchManager patches(master, store, leds);
        GlobalSettings g = default_globals();
        g.bpm = 88;
        TEST_ASSERT_EQUAL(APPLY_OK, patches.apply(three_node_patch(), g, 0));
        TEST_ASSERT_EQUAL(APPLY_OK, patches.save_slot(0));
        saved = patches.active();
    }
    {
        // A fresh everything, the same EEPROM: this is the power cycle.
        FakeGpio gpio; RecordingMidiOut midi; FakeLeds ld;
        MixedModeMaster master(gpio, midi);
        StatusLeds leds(ld);
        PatchStore store(eeprom);
        PatchManager patches(master, store, leds);
        patches.boot(0);
        TEST_ASSERT_FALSE(patches.running_defaults());
        TEST_ASSERT_TRUE(patches_equal(saved, patches.active()));
        TEST_ASSERT_EQUAL(88, master.clock().bpm());
        TEST_ASSERT_EQUAL(3, master.node_count());
    }
}

// A patch that is well-formed but does not validate never runs, and the one
// that was running is untouched.
static void test_an_invalid_patch_leaves_the_running_one_alone() {
    Rig rig;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(three_node_patch(), g, 0));

    Patch bad = empty_patch();
    bad.nodes[0] = node_config(200);                 // no such algorithm
    bad.n_nodes = 1;
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(bad, g, 1000));
    TEST_ASSERT_EQUAL(3, rig.master.node_count());
    TEST_ASSERT_EQUAL(ALGO_LOGIC_NOT, rig.patches.active().nodes[0].algorithm_id);
}

// Live parameter edits are mirrored into the active image, so what is saved
// is what is running - not what the patch said when it was loaded.
static void test_a_saved_patch_carries_live_parameter_edits() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(three_node_patch(), g, 0);
    TEST_ASSERT_EQUAL(PARAM_SET_OK, rig.patches.set_param(1, 3, 7, 1000));   // euclid pulses
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.save_slot(1));

    Patch back; GlobalSettings back_globals;
    TEST_ASSERT_EQUAL(STORE_OK, rig.store.load(1, back, back_globals));
    TEST_ASSERT_EQUAL(7, back.nodes[1].params[3]);
}

// A patch too large for a slot is refused rather than truncated, and what was
// in the slot survives.
static void test_a_patch_too_large_for_a_slot_is_refused() {
    FakeEeprom eeprom;
    PatchStore store(eeprom);
    GlobalSettings g = default_globals();
    store.save(0, three_node_patch(), g);

    // Poly sequencers are the widest nodes there are; enough of them exceed
    // a slot by a wide margin.
    Patch big = empty_patch();
    for (uint8_t n = 0; n < 8; n++) {
        big.nodes[n] = node_config(ALGO_POLY_SEQ);
        big.nodes[n].in_bus[0] = 0;
        big.nodes[n].out_bus[0] = 0;
        for (uint16_t p = 0; p < NoteSequencerBase::param_count(NOTE_SEQ_VOICES); p++) {
            big.nodes[n].params[p] = (uint8_t)(p | 1u);      // nothing to trim
        }
    }
    big.n_nodes = 8;
    TEST_ASSERT_EQUAL(STORE_TOO_LARGE, store.save(0, big, g));

    Patch back; GlobalSettings back_globals;
    TEST_ASSERT_EQUAL(STORE_OK, store.load(0, back, back_globals));
    TEST_ASSERT_TRUE(patches_equal(three_node_patch(), back));
}

// ---------------------------------------------------------------------------
// The LEDs.
// ---------------------------------------------------------------------------
static void test_green_flashes_on_the_beat_and_the_rate_follows_tempo() {
    FakeLeds driver;
    StatusLeds leds(driver);
    leds.set_clock_running(true);

    uint32_t now = 0;
    // Four beats, 500 ms apart (120 BPM).
    for (uint8_t b = 0; b < 4; b++) {
        leds.beat(now);
        leds.service(now);
        TEST_ASSERT_EQUAL(StatusLeds::BRIGHT, driver.levels[LED_GREEN]);
        leds.service(now + StatusLeds::BEAT_FLASH_US + 1);
        TEST_ASSERT_EQUAL(0, driver.levels[LED_GREEN]);
        now += 500000;
    }
    TEST_ASSERT_EQUAL(4, driver.rises(LED_GREEN));

    // Twice the tempo, twice the flashes in the same wall-clock time.
    driver.clear();
    for (uint8_t b = 0; b < 8; b++) {
        leds.beat(now);
        leds.service(now);
        leds.service(now + StatusLeds::BEAT_FLASH_US + 1);
        now += 250000;
    }
    TEST_ASSERT_EQUAL(8, driver.rises(LED_GREEN));
}

static void test_a_stopped_clock_still_shows_a_heartbeat() {
    FakeLeds driver;
    StatusLeds leds(driver);
    leds.set_clock_running(false);

    bool saw_dim = false, saw_off = false;
    for (uint32_t t = 0; t < StatusLeds::HEARTBEAT_PERIOD_US * 2u; t += 10000) {
        leds.service(t);
        if (driver.levels[LED_GREEN] == StatusLeds::DIM) saw_dim = true;
        if (driver.levels[LED_GREEN] == 0) saw_off = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_dim, "a module with no clock must still look alive");
    TEST_ASSERT_TRUE(saw_off);
    // And it is dim, not the bright beat flash: the two must read differently.
    TEST_ASSERT_TRUE(StatusLeds::DIM < StatusLeds::BRIGHT);
}

static void test_red_flashes_on_an_error_and_the_count_is_readable() {
    FakeLeds driver;
    StatusLeds leds(driver);
    leds.service(0);
    TEST_ASSERT_EQUAL(0, driver.levels[LED_RED]);

    leds.error(1000);
    leds.service(1000);
    TEST_ASSERT_EQUAL(StatusLeds::BRIGHT, driver.levels[LED_RED]);
    leds.service(1000 + StatusLeds::ERROR_FLASH_US + 1);
    TEST_ASSERT_EQUAL(0, driver.levels[LED_RED]);
    TEST_ASSERT_EQUAL(1, leds.errors());
}

static void test_identify_alternates_both_leds() {
    FakeLeds driver;
    StatusLeds leds(driver);
    leds.identify(0);

    bool green_alone = false, red_alone = false;
    for (uint32_t t = 0; t < (uint32_t)StatusLeds::IDENTIFY_BLINKS * StatusLeds::IDENTIFY_STEP_US; t += 10000) {
        leds.service(t);
        if (driver.levels[LED_GREEN] && !driver.levels[LED_RED]) green_alone = true;
        if (driver.levels[LED_RED] && !driver.levels[LED_GREEN]) red_alone = true;
    }
    TEST_ASSERT_TRUE(green_alone);
    TEST_ASSERT_TRUE(red_alone);
}

// ---------------------------------------------------------------------------
// The console. The key property is the last one: it works whatever patch is
// loaded, including one that failed to load at all.
// ---------------------------------------------------------------------------
static void test_console_dumps_the_running_patch() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(three_node_patch(), g, 0);

    rig.console.execute("patch", 0);
    TEST_ASSERT_TRUE(rig.io.said("EuclidianSequencer"));
    TEST_ASSERT_TRUE(rig.io.said("Transpose"));
    TEST_ASSERT_TRUE(rig.io.said("jack 1"));
}

static void test_console_reports_the_error_counters() {
    Rig rig;
    rig.patches.boot(0);
    rig.console.execute("errors", 0);
    TEST_ASSERT_TRUE(rig.io.said("note overflows"));
    TEST_ASSERT_TRUE(rig.io.said("rejected edges"));
    TEST_ASSERT_TRUE(rig.io.said("store writes"));
}

static void test_console_sets_and_gets_a_parameter() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(three_node_patch(), g, 0);

    rig.console.execute("set 1 3 9", 1000);
    TEST_ASSERT_TRUE(rig.io.said("ok, now 9"));
    rig.io.clear();
    rig.console.execute("get 1 3", 1000);
    TEST_ASSERT_TRUE(rig.io.said("9"));

    // A bad write is reported, not swallowed.
    rig.io.clear();
    rig.console.execute("set 1 3 200", 1000);
    TEST_ASSERT_TRUE(rig.io.said("outside the parameter's range"));
    rig.io.clear();
    rig.console.execute("set 9 0 1", 1000);
    TEST_ASSERT_TRUE(rig.io.said("no such node"));
}

static void test_console_lists_algorithms_and_parameters() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(three_node_patch(), g, 0);

    rig.console.execute("algos", 0);
    TEST_ASSERT_TRUE(rig.io.said("EuclidianSequencer"));
    TEST_ASSERT_TRUE(rig.io.said("DrumSeqMidi"));

    rig.io.clear();
    rig.console.execute("params 1", 0);
    TEST_ASSERT_TRUE(rig.io.said("pulses"));
    TEST_ASSERT_TRUE(rig.io.said("rotation"));
    TEST_ASSERT_TRUE(rig.io.said("direction"));
    TEST_ASSERT_TRUE(rig.io.said("forward"));      // the enum option name
}

static void test_console_saves_recalls_and_restores_defaults() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(three_node_patch(), g, 0);

    rig.console.execute("save 2", 0);
    TEST_ASSERT_TRUE(rig.io.said("saved"));

    rig.io.clear();
    rig.console.execute("defaults", 1000);
    TEST_ASSERT_TRUE(rig.io.said("built-in"));
    TEST_ASSERT_EQUAL(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id);

    rig.io.clear();
    rig.console.execute("load 2", 2000);
    TEST_ASSERT_TRUE(rig.io.said("loaded"));
    TEST_ASSERT_EQUAL(ALGO_LOGIC_NOT, rig.patches.active().nodes[0].algorithm_id);

    rig.io.clear();
    rig.console.execute("load 3", 3000);
    TEST_ASSERT_TRUE(rig.io.said("empty"));
    rig.io.clear();
    rig.console.execute("load 99", 3000);
    TEST_ASSERT_TRUE(rig.io.said("no such slot"));
}

// Typed a character at a time, with a backspace, the way a person does.
static void test_console_reads_a_line_from_the_transport() {
    Rig rig;
    rig.patches.boot(0);
    rig.io.type("infoo\b\r");       // "info", after a backspace over a typo
    rig.console.service(0);
    TEST_ASSERT_TRUE(rig.io.said("firmware"));
}

static void test_console_reports_an_unknown_command() {
    Rig rig;
    rig.patches.boot(0);
    rig.console.execute("frobnicate", 0);
    TEST_ASSERT_TRUE(rig.io.said("unknown command"));
}

// The lockout property, stated as a test: a patch that cannot be loaded does
// not take the console down with it. With no button to hold at power-on, a
// module that could be talked out of listening would need reflashing.
static void test_the_console_works_after_a_patch_fails_to_load() {
    Rig rig;
    GlobalSettings g = default_globals();

    Patch bad = empty_patch();
    bad.nodes[0] = node_config(ALGO_ARPEGGIATOR);     // needs two inlets, has none
    bad.n_nodes = 1;
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(bad, g, 0));

    rig.console.execute("info", 0);
    TEST_ASSERT_TRUE(rig.io.said("firmware"));
    rig.io.clear();
    rig.console.execute("errors", 0);
    TEST_ASSERT_TRUE(rig.io.said("last node error"));
    // And it can still accept the next good patch.
    rig.io.clear();
    rig.console.execute("defaults", 1000);
    TEST_ASSERT_TRUE(rig.io.said("built-in"));
}

// A line longer than the buffer is reported, not written past the end.
static void test_an_overlong_console_line_is_refused_not_overrun() {
    Rig rig;
    rig.patches.boot(0);
    for (int i = 0; i < Console::CONSOLE_LINE_MAX * 2; i++) rig.io.type("x");
    rig.io.type("\r");
    rig.console.service(0);
    TEST_ASSERT_TRUE(rig.io.said("line too long"));
}

// ---------------------------------------------------------------------------
// Nothing on the feedback surface allocates after boot.
// ---------------------------------------------------------------------------
static void test_leds_store_and_console_never_allocate() {
    Rig rig;
    rig.patches.boot(0);
    rig.console.execute("info", 0);           // warm anything that would allocate

    const size_t before = g_allocations;
    uint32_t now = 1000;
    for (int i = 0; i < 200; i++) {
        rig.leds.beat(now);
        rig.leds.service(now);
        rig.patches.service(now);
        now += 10000;
    }
    rig.console.execute("patch", now);
    rig.console.execute("errors", now);
    rig.console.execute("algos", now);
    rig.console.execute("set 0 0 64", now);
    rig.patches.save_slot(1);
    rig.patches.recall_slot(1, now);
    TEST_ASSERT_EQUAL(before, g_allocations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_patch_round_trips_through_the_codec);
    RUN_TEST(test_trailing_zero_parameters_are_not_stored);
    RUN_TEST(test_a_corrupted_image_is_refused);
    RUN_TEST(test_encoding_into_too_small_a_buffer_reports_rather_than_overruns);
    RUN_TEST(test_a_fresh_eeprom_reads_as_empty_not_corrupt);
    RUN_TEST(test_save_and_load_a_slot);
    RUN_TEST(test_a_corrupt_slot_does_not_take_the_others_with_it);
    RUN_TEST(test_erasing_a_slot_makes_it_empty_again);
    RUN_TEST(test_resaving_an_unchanged_patch_costs_no_writes);
    RUN_TEST(test_autosave_is_debounced);
    RUN_TEST(test_an_empty_module_boots_into_a_patch_that_does_something);
    RUN_TEST(test_an_empty_store_does_not_light_red);
    RUN_TEST(test_a_corrupt_store_boots_the_default_and_lights_red);
    RUN_TEST(test_a_saved_patch_survives_a_power_cycle);
    RUN_TEST(test_an_invalid_patch_leaves_the_running_one_alone);
    RUN_TEST(test_a_saved_patch_carries_live_parameter_edits);
    RUN_TEST(test_a_patch_too_large_for_a_slot_is_refused);
    RUN_TEST(test_green_flashes_on_the_beat_and_the_rate_follows_tempo);
    RUN_TEST(test_a_stopped_clock_still_shows_a_heartbeat);
    RUN_TEST(test_red_flashes_on_an_error_and_the_count_is_readable);
    RUN_TEST(test_identify_alternates_both_leds);
    RUN_TEST(test_console_dumps_the_running_patch);
    RUN_TEST(test_console_reports_the_error_counters);
    RUN_TEST(test_console_sets_and_gets_a_parameter);
    RUN_TEST(test_console_lists_algorithms_and_parameters);
    RUN_TEST(test_console_saves_recalls_and_restores_defaults);
    RUN_TEST(test_console_reads_a_line_from_the_transport);
    RUN_TEST(test_console_reports_an_unknown_command);
    RUN_TEST(test_the_console_works_after_a_patch_fails_to_load);
    RUN_TEST(test_an_overlong_console_line_is_refused_not_overrun);
    RUN_TEST(test_leds_store_and_console_never_allocate);
    return UNITY_END();
}
