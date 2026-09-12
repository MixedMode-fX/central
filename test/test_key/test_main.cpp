#include <unity.h>
#include <stdlib.h>
#include <new>
#include <vector>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "control/cc_mapper.h"
#include "control/nrpn.h"
#include "control/mod_matrix.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "node/registry.h"
#include "node/schedule.h"
#include "hal/midi_types.h"
#include "midi/global_key.h"
#include "midi/note_event.h"
#include "algorithm/midi/key.h"
#include "algorithm/midi/chord.h"
#include "algorithm/modulator/lfo.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// The key is process-wide, so every test leaves it as it found it.
void setUp() { global_key::set(SCALE_CHROMATIC, 0); }
void tearDown() { global_key::set(SCALE_CHROMATIC, 0); }

// The key, from inside the patch and from outside it: the Key node
// (algorithm/midi/key.h) and CC_TARGET_KEY (node/patch.h).

static constexpr uint8_t KEYBOARD = mmMIDI_SERIAL_1;
static constexpr uint8_t NOTE_ROOT = 0, NOTE_OUT = 1;

static MidiEvent on(uint8_t note, uint8_t channel = 1){
    return MidiEvent{MIDI_NOTE_ON, channel, note, 100};
}

// ---------------------------------------------------------------------------
// The Key node
// ---------------------------------------------------------------------------

static NodeConfig key_config(uint8_t channel = 0, uint8_t from = 0){
    NodeConfig c = node_config(ALGO_KEY);
    c.in_bus[0] = NOTE_ROOT;
    c.params[Key::P_CHANNEL] = channel;
    c.params[Key::P_FROM] = from;
    return c;
}

static void run(Key& node, BusManager& bus){
    bus.swap();
    node.process(bus, 0);
}

// The whole point: a note bus moves the key every other node plays in.
static void test_a_note_on_moves_the_keys_root() {
    BusManager bus;
    Key node(key_config());

    global_key::set(SCALE_NATURAL_MINOR, 0);
    bus.note_write(NOTE_ROOT, on(64));                 // E, in whatever octave
    run(node, bus);
    TEST_ASSERT_EQUAL(4, global_key::root());
    TEST_ASSERT_EQUAL(1, node.moves());
    // The scale is untouched: this inlet is a root, not a key change.
    TEST_ASSERT_EQUAL(SCALE_NATURAL_MINOR, global_key::id());

    // Last note-on in the pass wins, and a note-off never moves the key: a
    // key is a place the music is, not a note somebody is holding.
    bus.note_write(NOTE_ROOT, on(65));
    bus.note_write(NOTE_ROOT, on(67));
    bus.note_write(NOTE_ROOT, MidiEvent{MIDI_NOTE_OFF, 1, 67, 0});
    run(node, bus);
    TEST_ASSERT_EQUAL(7, global_key::root());

    // And nothing arriving leaves the key where the last note left it, so a
    // sequencer that moves it once a phrase need not hold a note.
    run(node, bus);
    TEST_ASSERT_EQUAL(7, global_key::root());
}

// The register is optional, because a melody written across two octaves
// would otherwise drag the whole patch up and down with it.
static void test_the_register_moves_only_when_it_is_asked_for() {
    BusManager bus;
    Key pitch_class(key_config(0, Key::KEY_FROM_PITCH_CLASS));

    global_key::set(SCALE_MAJOR, 0, 5);
    bus.note_write(NOTE_ROOT, on(38));                 // D1
    run(pitch_class, bus);
    TEST_ASSERT_EQUAL(2, global_key::root());
    TEST_ASSERT_EQUAL(5, global_key::octave());        // where it was

    BusManager other;
    Key whole_note(key_config(0, Key::KEY_FROM_NOTE));
    other.note_write(NOTE_ROOT, on(38));
    run(whole_note, other);
    TEST_ASSERT_EQUAL(2, global_key::root());
    TEST_ASSERT_EQUAL(3, global_key::octave());        // 38 / 12
    TEST_ASSERT_EQUAL(38, global_key::tonic(0));

    // Notes 0..11 sit in an octave the key cannot name, so they take the
    // lowest one it can rather than reading as "the default".
    other.note_write(NOTE_ROOT, on(5));
    run(whole_note, other);
    TEST_ASSERT_EQUAL(1, global_key::octave());
}

// A note bus carries channels, so a progression on one channel can move the
// key while the parts on the others do not.
static void test_a_channel_filter_keeps_the_other_parts_out_of_it() {
    BusManager bus;
    Key node(key_config(3));

    global_key::set(SCALE_MAJOR, 0);
    bus.note_write(NOTE_ROOT, on(62, 1));
    run(node, bus);
    TEST_ASSERT_EQUAL(0, global_key::root());
    TEST_ASSERT_EQUAL(0, node.moves());

    bus.note_write(NOTE_ROOT, on(62, 3));
    run(node, bus);
    TEST_ASSERT_EQUAL(2, global_key::root());
    TEST_ASSERT_EQUAL(1, node.moves());
}

// It holds no key of its own: what it writes is the key that is playing, and
// the settings a preset would save are left exactly where they were.
static void test_the_node_moves_the_live_key_and_not_the_stored_one() {
    FakeGpio gpio; RecordingMidiOut midi; FakeEeprom eeprom; FakeLeds driver;
    MixedModeMaster master(gpio, midi);
    StatusLeds leds(driver);
    PatchStore store(eeprom);
    PatchManager patches(master, store, leds);

    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, NOTE_ROOT};
    p.nodes[0] = key_config();
    p.n_nodes = 1;
    GlobalSettings g = default_globals();
    g.scale = SCALE_MAJOR;
    g.root = 0;
    TEST_ASSERT_EQUAL(APPLY_OK, patches.apply(p, g, 0));
    TEST_ASSERT_EQUAL(0, global_key::root());

    uint32_t now = 1000;
    master.deliver_midi(KEYBOARD, on(69), now);
    master.pass(now);
    TEST_ASSERT_EQUAL(9, global_key::root());          // the key that is playing
    TEST_ASSERT_EQUAL(0, patches.globals().root);      // the key that is stored
}

// ---------------------------------------------------------------------------
// One per patch
// ---------------------------------------------------------------------------

// The key has one value, so two nodes writing it would be two writers racing
// over it - the same reason two modulation routes may not share a target.
static void test_a_patch_may_hold_only_one_key_node() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);

    TEST_ASSERT_TRUE(registry::find(ALGO_KEY)->singleton);

    Patch one = empty_patch();
    one.nodes[0] = key_config();
    one.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(one));

    Patch two = empty_patch();
    two.nodes[0] = key_config();
    two.nodes[1] = key_config();
    two.n_nodes = 2;
    TEST_ASSERT_EQUAL(LOAD_DUPLICATE_SINGLETON, master.load(two));
    TEST_ASSERT_EQUAL(1, master.last_node_index());

    // The refused patch changed nothing: the one that was running still is.
    TEST_ASSERT_EQUAL(1, master.node_count());
}

// ---------------------------------------------------------------------------
// Where it runs
// ---------------------------------------------------------------------------

// A key change has to be heard by the notes of the same pass, not the next
// one: that is the bar-behind bug node/schedule.h exists to prevent, and the
// key is ordered by the same machinery as a bus (node/node.h).
static void test_the_key_node_runs_before_every_node_that_plays_in_the_key() {
    Schedule sched;
    // Deliberately the wrong way round in the patch: the Chord is node 0.
    NodeConfig chord = node_config(ALGO_CHORD);
    chord.in_bus[0] = NOTE_ROOT;
    chord.out_bus[0] = NOTE_OUT;
    NodeConfig key = key_config();
    sched.set(0, chord, *registry::find(ALGO_CHORD));
    sched.set(1, key, *registry::find(ALGO_KEY));
    sched.build();

    TEST_ASSERT_EQUAL(2, sched.count());
    TEST_ASSERT_EQUAL(1, sched.node_at(0));            // the Key node first
    TEST_ASSERT_EQUAL(0, sched.node_at(1));
}

// The whole path, in one pass: a note-on reaches the Key node over a bus, and
// the Chord downstream of the same bus voices the key it has just been moved
// to rather than the one it was in.
static void test_a_key_change_is_heard_in_the_pass_that_made_it() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);

    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, NOTE_ROOT};
    p.nodes[0] = node_config(ALGO_CHORD);              // listed first, runs second
    p.nodes[0].in_bus[0] = NO_BUS;                     // self-playing
    p.nodes[0].out_bus[0] = NOTE_OUT;
    p.nodes[0].params[Chord::P_OCTAVE] = 5;
    p.nodes[1] = key_config();
    p.n_nodes = 2;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    global_key::set(SCALE_MAJOR, 0);
    uint32_t now = 1000;
    master.pass(now); now += 1000;                     // C major triad

    master.deliver_midi(KEYBOARD, on(65), now);        // move the key to F
    master.pass(now);
    TEST_ASSERT_EQUAL(5, global_key::root());

    // The chord it is holding is the F one, in the pass the note arrived.
    uint8_t last_on = 0xFF;
    const uint8_t n = master.buses().note_count(NOTE_OUT);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = master.buses().note_read(NOTE_OUT, i);
        if (is_note_on(e) && last_on == 0xFF) last_on = e.data1;
    }
    TEST_ASSERT_EQUAL(65, last_on);
}

// ---------------------------------------------------------------------------
// The key as a control target: CC, NRPN and a modulation route
// ---------------------------------------------------------------------------

struct ControlRig {
    FakeGpio gpio;
    RecordingMidiOut midi;
    FakeEeprom eeprom;
    FakeLeds driver;
    MixedModeMaster master;
    StatusLeds leds;
    PatchStore store;
    PatchManager patches;
    CcMapper cc;

    ControlRig() : gpio(), midi(), eeprom(), driver(),
                   master(gpio, midi), leds(driver), store(eeprom),
                   patches(master, store, leds), cc(patches, master) {}
};

// A knob on the key: the thing the module could not do at all while the key
// was a parameter on eight different algorithms.
static void test_a_cc_moves_the_key() {
    ControlRig rig;
    Patch p = empty_patch();
    p.cc_map[0] = unused_mapping();
    p.cc_map[0].source_mask = KEYBOARD;
    p.cc_map[0].channel = 1;
    p.cc_map[0].cc = 20;
    p.cc_map[0].target_kind = CC_TARGET_KEY;
    p.cc_map[0].param = CC_KEY_ROOT;
    p.cc_map[1] = unused_mapping();
    p.cc_map[1].source_mask = KEYBOARD;
    p.cc_map[1].channel = 1;
    p.cc_map[1].cc = 21;
    p.cc_map[1].target_kind = CC_TARGET_KEY;
    p.cc_map[1].param = CC_KEY_SCALE;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));

    // The root sweeps the twelve pitch classes. A bound CC is consumed: the
    // user asked for a knob, not for the CC to also reach the graph.
    TEST_ASSERT_TRUE(rig.cc.observe(KEYBOARD, 1, 20, 127, 1000));
    rig.cc.apply(1000);
    TEST_ASSERT_EQUAL(11, global_key::root());
    TEST_ASSERT_EQUAL(11, rig.patches.globals().root);

    // The scale sweeps the real scales only: zero is an unset byte and not
    // somewhere a knob should be able to land.
    TEST_ASSERT_TRUE(rig.cc.observe(KEYBOARD, 1, 21, 0, 2000));
    rig.cc.apply(2000);
    TEST_ASSERT_EQUAL(SCALE_MAJOR, global_key::id());
    TEST_ASSERT_TRUE(rig.cc.observe(KEYBOARD, 1, 21, 127, 3000));
    rig.cc.apply(3000);
    TEST_ASSERT_EQUAL(SCALE_CHROMATIC, global_key::id());
}

// The key reads back live, which is not the same as what a preset holds: a
// Key node moves the first and leaves the second alone.
static void test_the_key_reads_back_and_ranges_like_any_other_target() {
    ControlRig rig;
    GlobalSettings g = default_globals();
    g.scale = SCALE_DORIAN;
    g.root = 4;
    g.root_octave = 7;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(empty_patch(), g, 0));

    uint16_t value = 0;
    TEST_ASSERT_TRUE(rig.cc.read_control(CC_TARGET_KEY, 0, CC_KEY_ROOT, value));
    TEST_ASSERT_EQUAL(4, value);
    TEST_ASSERT_TRUE(rig.cc.read_control(CC_TARGET_KEY, 0, CC_KEY_SCALE, value));
    TEST_ASSERT_EQUAL(SCALE_DORIAN, value);
    TEST_ASSERT_TRUE(rig.cc.read_control(CC_TARGET_KEY, 0, CC_KEY_OCTAVE, value));
    TEST_ASSERT_EQUAL(7, value);
    TEST_ASSERT_FALSE(rig.cc.read_control(CC_TARGET_KEY, 0, CC_KEY_TARGETS, value));

    uint16_t lo = 0, hi = 0;
    TEST_ASSERT_TRUE(rig.cc.target_range(CC_TARGET_KEY, 0, CC_KEY_ROOT, lo, hi));
    TEST_ASSERT_EQUAL(0, lo); TEST_ASSERT_EQUAL(11, hi);
    TEST_ASSERT_TRUE(rig.cc.target_range(CC_TARGET_KEY, 0, CC_KEY_SCALE, lo, hi));
    TEST_ASSERT_EQUAL(SCALE_MAJOR, lo); TEST_ASSERT_EQUAL(SCALE_COUNT - 1, hi);
    TEST_ASSERT_TRUE(rig.cc.target_range(CC_TARGET_KEY, 0, CC_KEY_OCTAVE, lo, hi));
    TEST_ASSERT_EQUAL(1, lo); TEST_ASSERT_EQUAL(global_key::MAX_OCTAVE, hi);
    TEST_ASSERT_FALSE(rig.cc.target_range(CC_TARGET_KEY, 0, CC_KEY_TARGETS, lo, hi));
}

// The key has its own block of the NRPN address space, and it round-trips.
static void test_the_key_has_an_nrpn_address() {
    uint8_t kind = 0, index = 0;
    uint16_t param = 0, address = 0;

    for (uint16_t target = 0; target < CC_KEY_TARGETS; target++){
        TEST_ASSERT_TRUE(NrpnDecoder::address_of(CC_TARGET_KEY, 0, target, address));
        TEST_ASSERT_EQUAL(NRPN_KEY_BASE + target, address);
        TEST_ASSERT_TRUE(NrpnDecoder::resolve(address, kind, index, param));
        TEST_ASSERT_EQUAL(CC_TARGET_KEY, kind);
        TEST_ASSERT_EQUAL(target, param);
    }
    TEST_ASSERT_FALSE(NrpnDecoder::address_of(CC_TARGET_KEY, 0, CC_KEY_TARGETS, address));
    TEST_ASSERT_FALSE(NrpnDecoder::resolve(NRPN_KEY_BASE + CC_KEY_TARGETS, kind, index, param));
    TEST_ASSERT_FALSE(NrpnDecoder::resolve(NRPN_RESERVED_BASE, kind, index, param));
}

// A modulation route reaches the key like anything else, and writes it
// transiently: an LFO walking the key runs for hours, and a write that armed
// the autosave every pass would rewrite slot 0 to EEPROM until the module was
// switched off.
static void test_a_modulation_route_moves_the_key_without_wearing_the_store() {
    ControlRig rig;
    ModMatrix matrix(rig.patches, rig.cc);

    Patch p = empty_patch();
    p.mod_map[0] = unused_route();
    p.mod_map[0].bus = 0;
    p.mod_map[0].target_kind = CC_TARGET_KEY;
    p.mod_map[0].param = CC_KEY_ROOT;
    p.mod_map[0].depth = 255;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));

    // Let the autosave the patch load armed settle, so what is measured
    // below is the route's own writes and nothing else.
    rig.patches.service(60000000u);
    TEST_ASSERT_FALSE(rig.store.dirty());

    BusManager bus;
    bus.cv_write(0, CV_MAX);
    bus.swap();
    matrix.apply(bus, 60001000u);
    TEST_ASSERT_EQUAL(11, global_key::root());          // the key that is playing
    TEST_ASSERT_FALSE(rig.store.dirty());               // and no flash write for it

    // A field the key does not have is refused, as any other target's would
    // be: a route that quietly wrote nowhere would be worse than one refused.
    Patch bad = empty_patch();
    bad.mod_map[0] = p.mod_map[0];
    bad.mod_map[0].param = CC_KEY_TARGETS;
    TEST_ASSERT_EQUAL(LOAD_MOD_ROUTE_INVALID, rig.master.load(bad));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_note_on_moves_the_keys_root);
    RUN_TEST(test_the_register_moves_only_when_it_is_asked_for);
    RUN_TEST(test_a_channel_filter_keeps_the_other_parts_out_of_it);
    RUN_TEST(test_the_node_moves_the_live_key_and_not_the_stored_one);
    RUN_TEST(test_a_patch_may_hold_only_one_key_node);
    RUN_TEST(test_the_key_node_runs_before_every_node_that_plays_in_the_key);
    RUN_TEST(test_a_key_change_is_heard_in_the_pass_that_made_it);
    RUN_TEST(test_a_cc_moves_the_key);
    RUN_TEST(test_the_key_reads_back_and_ranges_like_any_other_target);
    RUN_TEST(test_the_key_has_an_nrpn_address);
    RUN_TEST(test_a_modulation_route_moves_the_key_without_wearing_the_store);
    return UNITY_END();
}
