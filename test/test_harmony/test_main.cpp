#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "algorithm/midi/harmony.h"
#include "algorithm/midi/chord.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() { global_scale::set(SCALE_MAJOR, 0); }
void tearDown() { global_scale::set(SCALE_CHROMATIC, 0); }

// Harmony (#33): the module had a key and nothing that moved inside it.

static const uint8_t GATE_ADVANCE = 0, GATE_RESET = 1;
static const uint8_t NOTE_ROOT = 0, NOTE_CHORD = 1;
static const uint8_t CV_DEGREE = 0;

// `cadence` of 1 rather than 0 is how a test asks for "no cadence": a stored
// 0 means the descriptor's default everywhere in this module, and 1% over any
// phrase count anyone would use is the same thing as none. `gravity` needs no
// such dodge, which is why it is named for the pull - see the header.
static NodeConfig harmony_config(uint8_t style, uint8_t phrase, uint8_t cadence,
                                 uint8_t gravity, uint8_t seed){
    NodeConfig c = node_config(ALGO_HARMONY);
    c.in_bus[0] = GATE_ADVANCE;
    c.in_bus[1] = GATE_RESET;
    c.out_bus[0] = NOTE_ROOT;
    c.out_bus[1] = CV_DEGREE;
    c.params[0] = style;
    c.params[1] = phrase;
    c.params[2] = cadence;
    c.params[3] = gravity;
    c.params[5] = 48;            // C3
    c.params[9] = seed;
    return c;
}

static void idle(Harmony& node, BusManager& bus, uint32_t& now){
    bus.gate_write(GATE_ADVANCE, false);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;
}

static void advance(Harmony& node, BusManager& bus, uint32_t& now){
    idle(node, bus, now);
    bus.gate_write(GATE_ADVANCE, true);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;
}

static void pulse_reset(Harmony& node, BusManager& bus, uint32_t& now){
    bus.gate_write(GATE_RESET, true);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;
    bus.gate_write(GATE_RESET, false);
    bus.swap();
    node.process(bus, now);
    bus.swap();
    now += 1000;
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------

static void test_nothing_sounds_until_the_first_advance_and_then_the_tonic() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_POP, 4, 75, 0, 3);
    Harmony node(c);
    uint32_t now = 0;

    for (uint8_t i = 0; i < 5; i++) idle(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(0xFF, node.playing());
    TEST_ASSERT_EQUAL_UINT8(0xFF, node.degree());

    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(0, node.degree());
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());       // C3, the tonic
}

static void test_every_root_is_a_degree_of_the_key() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 8, 1, 0, 11);
    Harmony node(c);
    uint32_t now = 0;
    const uint16_t mask = scale_mask(SCALE_MAJOR);

    for (uint32_t i = 0; i < 300; i++){
        advance(node, bus, now);
        const uint8_t p = node.playing();
        TEST_ASSERT_TRUE(p <= 127);
        const uint8_t pc = (uint8_t)(((int16_t)p - 48 + 120) % 12);
        TEST_ASSERT_TRUE_MESSAGE(mask & (uint16_t)(1u << pc), "a root outside the key");
    }
}

static void test_full_gravity_never_leaves_the_tonic() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_JAZZ, 8, 1, 100, 5);
    Harmony node(c);
    uint32_t now = 0;
    for (uint32_t i = 0; i < 200; i++){
        advance(node, bus, now);
        TEST_ASSERT_EQUAL_UINT8(0, node.degree());
        TEST_ASSERT_EQUAL_UINT8(48, node.playing());
    }
}

static void test_a_full_cadence_resolves_every_phrase() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_JAZZ, 4, 100, 0, 5);
    Harmony node(c);
    uint32_t now = 0;

    advance(node, bus, now);                      // chord 1 of phrase 1: the tonic
    for (uint32_t p = 0; p < 20; p++){
        advance(node, bus, now);                  // 2
        advance(node, bus, now);                  // 3
        advance(node, bus, now);                  // 4 - the last of the phrase
        TEST_ASSERT_EQUAL_MESSAGE(0, node.degree(), "a phrase that did not resolve");
        advance(node, bus, now);                  // 1 of the next phrase
    }
}

static void test_no_cadence_lets_a_phrase_end_anywhere() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 4, 1, 0, 5);
    Harmony node(c);
    uint32_t now = 0;
    advance(node, bus, now);

    uint32_t off_tonic = 0;
    for (uint32_t p = 0; p < 40; p++){
        advance(node, bus, now);
        advance(node, bus, now);
        advance(node, bus, now);
        if (node.degree() != 0) off_tonic++;
        advance(node, bus, now);
    }
    TEST_ASSERT_TRUE_MESSAGE(off_tonic > 20, "cadence 0 still resolved every phrase");
}

static void test_pedal_stays_home_far_more_than_a_uniform_walk() {
    BusManager bus;
    uint32_t home[2] = {0, 0};
    const uint8_t styles[2] = {Harmony::HARM_PEDAL, Harmony::HARM_WALK};
    for (uint8_t s = 0; s < 2; s++){
        NodeConfig c = harmony_config(styles[s], 16, 1, 0, 21);
        Harmony node(c);
        uint32_t now = 0;
        for (uint32_t i = 0; i < 400; i++){
            advance(node, bus, now);
            if (node.degree() == 0) home[s]++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(home[0] > home[1] * 3, "pedal did not stay home");
}

static void test_jazz_falls_by_fifths() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_JAZZ, 16, 1, 0, 31);
    Harmony node(c);
    uint32_t now = 0;
    advance(node, bus, now);

    // Down a fifth is three degrees up. The claim is not that jazz only ever
    // does that - it is that the move is what the style is built out of, so a
    // histogram of every transition should have one obvious peak.
    uint32_t moves[7] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t previous = node.degree();
    for (uint32_t i = 0; i < 600; i++){
        advance(node, bus, now);
        const uint8_t d = node.degree();
        moves[(d + 7u - previous) % 7u]++;
        previous = d;
    }
    uint32_t runner_up = 0;
    for (uint8_t m = 0; m < 7; m++) if (m != 3 && moves[m] > runner_up) runner_up = moves[m];
    TEST_ASSERT_TRUE_MESSAGE(moves[3] > 600 / 2, "the fifth fall was not most of it");
    TEST_ASSERT_TRUE_MESSAGE(moves[3] > runner_up * 3, "the fifth fall was not the peak");
}

// ---------------------------------------------------------------------------
// The reason it only has to emit a root
// ---------------------------------------------------------------------------

// Chord's intervals are steps of the scale, so a triad voiced on any degree
// of the key is already the right *quality*: major on I, minor on ii,
// diminished on vii. Harmony knows nothing about chord quality and never
// needs to - this is the test that says so out loud.
static void test_a_triad_on_every_degree_comes_out_the_right_quality() {
    BusManager bus;
    NodeConfig hc = harmony_config(Harmony::HARM_WALK, 16, 1, 0, 7);
    Harmony harmony(hc);

    NodeConfig cc = node_config(ALGO_CHORD);
    cc.in_bus[0] = NO_BUS;                 // nothing played: the chord follows its root inlet
    cc.in_bus[1] = NOTE_ROOT;
    cc.out_bus[0] = NOTE_CHORD;
    cc.params[0] = 3;                      // three voices
    cc.params[1] = 0;                      // the root
    cc.params[2] = 2;                      // a third of the scale
    cc.params[3] = 4;                      // a fifth of the scale
    Chord chord(cc);

    uint32_t now = 0;
    bool seen_major = false, seen_minor = false, seen_diminished = false;
    uint8_t degrees_seen = 0;
    bool degree_seen[7] = {false, false, false, false, false, false, false};

    for (uint32_t i = 0; i < 400; i++){
        // `advance` leaves the harmony's writes published, so the chord reads
        // them on the pass after - the module's one pass of latency per
        // stage - and one swap publishes what it voiced.
        advance(harmony, bus, now);
        const uint8_t deg = harmony.degree();
        const uint8_t root = harmony.playing();
        chord.process(bus, now);
        bus.swap();

        uint8_t voices[8];
        uint8_t n = 0;
        for (uint8_t k = 0; k < bus.note_count(NOTE_CHORD); k++){
            const MidiEvent e = bus.note_read(NOTE_CHORD, k);
            if (is_note_on(e) && n < 8) voices[n++] = e.data1;
        }
        if (n != 3) continue;              // the pass the chord was released on

        // The voices are the root and two intervals above it, and the shape
        // of those intervals is what makes the chord major, minor or
        // diminished. Nothing in Harmony chose it.
        uint8_t low = voices[0], mid = voices[1], high = voices[2];
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(root, low, "the chord was not voiced on the root");
        const uint8_t third = (uint8_t)(mid - low);
        const uint8_t fifth = (uint8_t)(high - mid);
        if (!degree_seen[deg]){ degree_seen[deg] = true; degrees_seen++; }

        if (third == 4 && fifth == 3) seen_major = true;
        else if (third == 3 && fifth == 4) seen_minor = true;
        else if (third == 3 && fifth == 3) seen_diminished = true;
        else TEST_FAIL_MESSAGE("a diatonic triad that was none of the three");

        // And the quality is the one the degree of a major scale has.
        const bool major_degree = (deg == 0 || deg == 3 || deg == 4);
        const bool diminished_degree = (deg == 6);
        if (major_degree) TEST_ASSERT_EQUAL_UINT8_MESSAGE(4, third, "I, IV and V are major");
        else if (diminished_degree) TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, fifth, "vii is diminished");
        else TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, third, "ii, iii and vi are minor");
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(7, degrees_seen, "the walk did not reach every degree");
    TEST_ASSERT_TRUE(seen_major && seen_minor && seen_diminished);
}

// ---------------------------------------------------------------------------
// Phrase, loop, reset, key
// ---------------------------------------------------------------------------

static void test_loop_keeps_the_first_phrase_and_repeats_it() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 4, 1, 0, 9);
    c.params[4] = 1;                       // loop
    Harmony node(c);
    uint32_t now = 0;

    uint8_t phrase[4];
    for (uint8_t i = 0; i < 4; i++){ advance(node, bus, now); phrase[i] = node.degree(); }
    TEST_ASSERT_EQUAL_UINT8(4, node.recorded_chords());

    for (uint8_t round = 0; round < 20; round++){
        for (uint8_t i = 0; i < 4; i++){
            advance(node, bus, now);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(phrase[i], node.degree(), "the loop drifted");
        }
    }

    // Turning it off walks again, from wherever the loop left it.
    TEST_ASSERT_TRUE(node.set_param(4, 0));
    uint8_t different = 0;
    for (uint8_t round = 0; round < 20; round++){
        for (uint8_t i = 0; i < 4; i++){
            advance(node, bus, now);
            if (node.degree() != phrase[i]) different++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(different > 0, "loop off still played the loop");
}

static void test_switching_loop_on_waits_for_the_top_of_a_phrase() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 4, 1, 0, 17);
    Harmony node(c);
    uint32_t now = 0;

    advance(node, bus, now);               // phrase position is now 1
    advance(node, bus, now);               // ... 2
    TEST_ASSERT_EQUAL_UINT8(2, node.phrase_position());
    TEST_ASSERT_TRUE(node.set_param(4, 1));

    // Nothing is recorded until the phrase comes round, so what is captured
    // is a phrase and not the tail of one.
    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(0, node.recorded_chords());
    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(0, node.recorded_chords());
    advance(node, bus, now);               // position 0: recording starts here
    TEST_ASSERT_EQUAL_UINT8(1, node.recorded_chords());
}

static void test_reset_starts_the_phrase_again_on_the_tonic() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 8, 1, 0, 23);
    Harmony node(c);
    uint32_t now = 0;

    for (uint8_t i = 0; i < 5; i++) advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(5, node.phrase_position());

    pulse_reset(node, bus, now);
    // Reset does not silence: a root that dropped out until the next chord
    // would be a hole in the music, and every sequencer here agrees.
    TEST_ASSERT_TRUE(node.playing() != 0xFF);
    TEST_ASSERT_EQUAL_UINT8(0, node.phrase_position());

    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(0, node.degree());
    TEST_ASSERT_EQUAL_UINT8(1, node.phrase_position());
}

static void test_the_key_moving_transposes_the_progression() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 8, 1, 100, 5);   // pinned to the tonic
    Harmony node(c);
    uint32_t now = 0;

    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());       // C3 in C major

    // The key moves under a sounding root. The root follows - that is the
    // whole point of a key - and the release comes from the ledger, so the
    // note-off carries C3 and nothing hangs.
    global_scale::set(SCALE_NATURAL_MINOR, 9);         // A minor
    idle(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(57, node.playing());       // A3: the octave is this node's

    bool released_c3 = false;
    for (uint8_t k = 0; k < bus.note_count(NOTE_ROOT); k++){
        const MidiEvent e = bus.note_read(NOTE_ROOT, k);
        if (is_note_off(e) && e.data1 == 48) released_c3 = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(released_c3, "the old root was left hanging");
}

static void test_a_key_with_five_notes_has_five_chords() {
    BusManager bus;
    global_scale::set(SCALE_PENTATONIC_MINOR, 0);
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 16, 1, 0, 29);
    Harmony node(c);
    uint32_t now = 0;
    TEST_ASSERT_EQUAL_UINT8(5, node.usable_degrees());

    bool seen[7] = {false, false, false, false, false, false, false};
    for (uint32_t i = 0; i < 300; i++){
        advance(node, bus, now);
        TEST_ASSERT_TRUE_MESSAGE(node.degree() < 5, "a degree the key does not have");
        seen[node.degree()] = true;
    }
    for (uint8_t d = 0; d < 5; d++) TEST_ASSERT_TRUE(seen[d]);
}

static void test_the_degree_outlet_moves_with_the_chord() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 16, 1, 0, 33);
    Harmony node(c);
    uint32_t now = 0;
    for (uint32_t i = 0; i < 60; i++){
        advance(node, bus, now);
        const int16_t expect = (int16_t)(((int32_t)node.degree() * CV_MAX) / 6);
        TEST_ASSERT_EQUAL_INT16(expect, bus.cv_read(CV_DEGREE));
    }
}

static void test_a_patch_swap_releases_the_root() {
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_WALK, 4, 1, 100, 5);
    Harmony node(c);
    uint32_t now = 0;
    advance(node, bus, now);
    node.silence(bus);
    bus.swap();
    TEST_ASSERT_EQUAL_UINT8(1, bus.note_count(NOTE_ROOT));
    TEST_ASSERT_TRUE(is_note_off(bus.note_read(NOTE_ROOT, 0)));
    TEST_ASSERT_EQUAL_UINT8(48, bus.note_read(NOTE_ROOT, 0).data1);
}

static void test_one_seed_is_one_progression() {
    BusManager bus_a, bus_b;
    NodeConfig a = harmony_config(Harmony::HARM_POP, 8, 50, 0, 44);
    NodeConfig b = harmony_config(Harmony::HARM_POP, 8, 50, 0, 44);
    Harmony one(a), two(b);
    uint32_t now_a = 0, now_b = 0;
    for (uint32_t i = 0; i < 100; i++){
        advance(one, bus_a, now_a);
        advance(two, bus_b, now_b);
        TEST_ASSERT_EQUAL_UINT8(one.degree(), two.degree());
    }
}

static void test_harmony_never_allocates() {
    const size_t before = g_allocations;
    BusManager bus;
    NodeConfig c = harmony_config(Harmony::HARM_POP, 4, 75, 0, 5);
    Harmony node(c);
    uint32_t now = 0;
    for (uint32_t i = 0; i < 300; i++) advance(node, bus, now);
    TEST_ASSERT_EQUAL_size_t(before, g_allocations);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_sounds_until_the_first_advance_and_then_the_tonic);
    RUN_TEST(test_every_root_is_a_degree_of_the_key);
    RUN_TEST(test_full_gravity_never_leaves_the_tonic);
    RUN_TEST(test_a_full_cadence_resolves_every_phrase);
    RUN_TEST(test_no_cadence_lets_a_phrase_end_anywhere);
    RUN_TEST(test_pedal_stays_home_far_more_than_a_uniform_walk);
    RUN_TEST(test_jazz_falls_by_fifths);
    RUN_TEST(test_a_triad_on_every_degree_comes_out_the_right_quality);
    RUN_TEST(test_loop_keeps_the_first_phrase_and_repeats_it);
    RUN_TEST(test_switching_loop_on_waits_for_the_top_of_a_phrase);
    RUN_TEST(test_reset_starts_the_phrase_again_on_the_tonic);
    RUN_TEST(test_the_key_moving_transposes_the_progression);
    RUN_TEST(test_a_key_with_five_notes_has_five_chords);
    RUN_TEST(test_the_degree_outlet_moves_with_the_chord);
    RUN_TEST(test_a_patch_swap_releases_the_root);
    RUN_TEST(test_one_seed_is_one_progression);
    RUN_TEST(test_harmony_never_allocates);
    return UNITY_END();
}
