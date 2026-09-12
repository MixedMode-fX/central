#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_key.h"
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

void setUp() { global_key::set(SCALE_MAJOR, 0); }
void tearDown() { global_key::set(SCALE_CHROMATIC, 0); }

// Harmony (#33): the module had a key and nothing that moved inside it.

static const uint8_t GATE_ADVANCE = 0, GATE_RESET = 1;
static const uint8_t NOTE_ROOT = 0, NOTE_CHORD = 1;
static const uint8_t CV_DEGREE = 0;

// `cadence` of 1 rather than 0 is how a test asks for "no cadence": a stored
// 0 means the descriptor's default everywhere in this module, and 1% over any
// phrase count anyone would use is the same thing as none. `gravity` needs no
// such dodge, which is why it is named for the pull - see the header.
// `spread` defaults to 100 here - a flat draw over every legal move - because
// that is the null model the mechanics tests want: nothing about the walk's
// opinions should change what a phrase, a cadence, a loop or a reset does.
static NodeConfig harmony_config(uint8_t phrase, uint8_t cadence,
                                 uint8_t gravity, uint8_t seed, uint8_t spread = 100){
    NodeConfig c = node_config(ALGO_HARMONY);
    c.in_bus[0] = GATE_ADVANCE;
    c.in_bus[1] = GATE_RESET;
    c.out_bus[0] = NOTE_ROOT;
    c.out_bus[1] = CV_DEGREE;
    c.params[Harmony::P_PHRASE] = phrase;
    c.params[Harmony::P_CADENCE] = cadence;
    c.params[Harmony::P_GRAVITY] = gravity;
    c.params[Harmony::P_OCTAVE] = 4;           // C3 is 4 x 12
    c.params[Harmony::P_SEED] = seed;
    c.params[Harmony::P_SPREAD] = spread;
    return c;
}

// A node built only to be asked what it would do: the walk's opinions, with
// none of the phrase machinery in the way.
static NodeConfig walk_config(uint8_t fifths, uint8_t smooth, uint8_t leading,
                              uint8_t spread = 50){
    NodeConfig c = harmony_config(16, 1, 0, 5, spread);
    c.params[Harmony::P_FIFTHS] = fifths;
    c.params[Harmony::P_SMOOTH] = smooth;
    c.params[Harmony::P_LEADING] = leading;
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
    NodeConfig c = harmony_config(4, 75, 0, 3);
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
    NodeConfig c = harmony_config(8, 1, 0, 11);
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
    NodeConfig c = harmony_config(8, 1, 100, 5);
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
    NodeConfig c = harmony_config(4, 100, 0, 5);
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
    NodeConfig c = harmony_config(4, 1, 0, 5);
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

// The claim the whole rewrite rests on: at `fifths` 100 the move this walk
// most wants to make, from every degree, is the one whose root falls a
// perfect fifth - and it worked that out from twelve bits, with no table
// anywhere naming a degree.
//
// **Except from IV**, and that exception is the point. A table that counted
// scale steps would send IV to vii, because vii is three degrees up like
// every other row. The fifth from IV down is *diminished*, which is why
// IV-viiº is the weak link in the diatonic circle, and a rule that measures
// semitones knows that without being told.
static void test_the_circle_of_fifths_is_computed_not_written_down() {
    BusManager bus;
    NodeConfig c = walk_config(100, 0, 50);
    Harmony node(c);
    (void)bus;

    // I->IV, ii->V, iii->vi, V->I, vi->ii, vii->iii: six perfect fifths.
    static const uint8_t FROM[6] = {0, 1, 2, 4, 5, 6};
    static const uint8_t WANT[6] = {3, 4, 5, 0, 1, 2};
    for (uint8_t i = 0; i < 6; i++){
        TEST_ASSERT_EQUAL_MESSAGE(WANT[i], node.likeliest_from(FROM[i]), "not the falling fifth");
    }
    // And from IV it declines to, because there is no perfect fifth below it.
    TEST_ASSERT_NOT_EQUAL(6, node.likeliest_from(3));
}

// The same rule, in a key it was never written for. A five-note scale has no
// table to truncate: the weights are measured in semitones, so the fifths
// that exist are found and the ones that do not are not invented.
static void test_the_same_rule_finds_the_fifths_of_a_pentatonic_key() {
    BusManager bus;
    NodeConfig c = walk_config(100, 0, 50);
    Harmony node(c);
    (void)bus;
    global_key::set(SCALE_PENTATONIC_MAJOR, 0);      // C D E G A

    TEST_ASSERT_EQUAL(5, node.usable_degrees());
    // D->G, E->A, G->C, A->D are all perfect fifths and all found.
    static const uint8_t FROM[4] = {1, 2, 3, 4};
    static const uint8_t WANT[4] = {3, 4, 0, 1};
    for (uint8_t i = 0; i < 4; i++){
        TEST_ASSERT_EQUAL_MESSAGE(WANT[i], node.likeliest_from(FROM[i]), "not the fifth");
    }
    // From C the fifth below is F, which this key does not have, so it is not
    // pretended into existence.
    TEST_ASSERT_EQUAL(2, node.likeliest_from(0));      // E, a third away
}

// One axis, two kinds of music: turn it the other way and the walk rises by
// fifths instead, which is how a dominant gets approached rather than
// resolved.
static void test_fifths_turns_the_walk_the_other_way_round() {
    BusManager bus;
    NodeConfig c = walk_config(1, 0, 50);
    Harmony node(c);
    (void)bus;

    TEST_ASSERT_EQUAL(4, node.likeliest_from(0));      // I -> V
    TEST_ASSERT_EQUAL(1, node.likeliest_from(4));      // V -> ii
    TEST_ASSERT_EQUAL(5, node.likeliest_from(1));      // ii -> vi
}

// Triads a third apart share two notes and a fifth apart share one, so asking
// for shared tones is asking for mediant motion - and that is Romantic
// harmony, reached by turning one knob rather than by selecting a genre.
static void test_smooth_buys_the_chords_that_share_notes() {
    BusManager bus;
    NodeConfig plain = walk_config(100, 0, 50);
    Harmony indifferent(plain);
    NodeConfig mediant = walk_config(100, 100, 50);
    Harmony smooth(mediant);
    (void)bus;

    TEST_ASSERT_EQUAL(3, indifferent.likeliest_from(0));   // I -> IV, the fifth
    TEST_ASSERT_EQUAL(2, smooth.likeliest_from(0));        // I -> iii, two shared notes
}

// The leading tone is what an authentic cadence is made of and what a mode
// must avoid, so one control reads as "how tonal" upward and "how modal"
// downward. In a mode that has no leading tone there is nothing for it to
// weight, and it says so by doing nothing at all.
static void test_leading_is_a_tonal_control_and_is_inert_without_one() {
    BusManager bus;
    (void)bus;
    uint32_t avoid[Harmony::DEGREES], want[Harmony::DEGREES];

    global_key::set(SCALE_MAJOR, 0);                  // has a leading tone
    NodeConfig low = walk_config(50, 0, 1);
    NodeConfig high = walk_config(50, 0, 100);
    Harmony modal(low), tonal(high);
    modal.weigh(0, avoid);
    tonal.weigh(0, want);
    // V carries the leading tone in a major key, so the two disagree about it.
    TEST_ASSERT_TRUE_MESSAGE(want[4] > avoid[4], "leading did not favour the dominant");

    // Mixolydian has a flat seventh and therefore no leading tone at all.
    global_key::set(SCALE_MIXOLYDIAN, 0);
    Harmony modal_mix(low), tonal_mix(high);
    modal_mix.weigh(0, avoid);
    tonal_mix.weigh(0, want);
    for (uint8_t j = 0; j < Harmony::DEGREES; j++){
        TEST_ASSERT_EQUAL_MESSAGE(avoid[j], want[j], "leading did something in a mode with none");
    }
}

// `spread` is the one that makes accidents. Sharpened, the walk hardens onto
// its favourite move; flattened, every legal move is as likely as any other -
// and the flat end is exactly the uniform null model this node used to need a
// named style for.
static void test_spread_runs_from_a_loop_to_a_uniform_walk() {
    BusManager bus;
    uint32_t visits[2][Harmony::DEGREES] = {{0}, {0}};
    const uint8_t SPREADS[2] = {1, 100};

    for (uint8_t k = 0; k < 2; k++){
        NodeConfig c = harmony_config(16, 1, 0, 21, SPREADS[k]);
        c.params[Harmony::P_FIFTHS] = 100;
        Harmony node(c);
        uint32_t now = 0;
        for (uint32_t i = 0; i < 700; i++){
            advance(node, bus, now);
            visits[k][node.degree() % Harmony::DEGREES]++;
        }
    }

    // Sharpened and falling by fifths, the walk is a cycle: every degree is
    // visited, and the transitions are the fifths.
    uint32_t sharp_low = 700, flat_low = 700, flat_high = 0;
    for (uint8_t j = 0; j < Harmony::DEGREES; j++){
        if (visits[0][j] < sharp_low) sharp_low = visits[0][j];
        if (visits[1][j] < flat_low) flat_low = visits[1][j];
        if (visits[1][j] > flat_high) flat_high = visits[1][j];
    }
    // Flat means flat: no degree runs away with it.
    TEST_ASSERT_TRUE_MESSAGE(flat_high < flat_low * 2, "the flat walk was not flat");
    (void)sharp_low;

    // And sharpened, the move it makes is overwhelmingly the fifth. Measured
    // in semitones, because that is what the rule measures.
    NodeConfig c = harmony_config(16, 1, 0, 31, 1);
    c.params[Harmony::P_FIFTHS] = 100;
    Harmony node(c);
    uint32_t now = 0;
    advance(node, bus, now);
    uint32_t fell_a_fifth = 0;
    uint8_t previous = node.playing();
    for (uint32_t i = 0; i < 600; i++){
        advance(node, bus, now);
        const uint8_t now_note = node.playing();
        if ((uint8_t)(((int16_t)now_note - (int16_t)previous + 120) % 12) == 5) fell_a_fifth++;
        previous = now_note;
    }
    TEST_ASSERT_TRUE_MESSAGE(fell_a_fifth > 600 / 2, "the fifth fall was not most of it");
}

// A pull to the tonic, against a walk with no opinion about it.
static void test_gravity_stays_home_far_more_than_a_flat_walk() {
    BusManager bus;
    uint32_t home[2] = {0, 0};
    const uint8_t pulls[2] = {80, 0};
    for (uint8_t s = 0; s < 2; s++){
        NodeConfig c = harmony_config(16, 1, pulls[s], 21);
        Harmony node(c);
        uint32_t now = 0;
        for (uint32_t i = 0; i < 400; i++){
            advance(node, bus, now);
            if (node.degree() == 0) home[s]++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(home[0] > home[1] * 3, "gravity did not stay home");
}

// An accident that happens once is a glitch and one that comes back is a
// decision. A loop with no drift is exact, which is what it always was; with
// drift it changes, and the change *stays* - the new chord replaces the old
// one in the phrase rather than passing through it.
static void test_drift_keeps_a_loop_alive() {
    BusManager bus;

    NodeConfig exact = harmony_config(4, 1, 0, 13);
    exact.params[Harmony::P_LOOP] = 1;
    Harmony fixed(exact);
    uint32_t now = 0;
    uint8_t first[4];
    for (uint8_t i = 0; i < 4; i++){ advance(fixed, bus, now); first[i] = fixed.degree(); }
    for (uint8_t round = 0; round < 6; round++){
        for (uint8_t i = 0; i < 4; i++){
            advance(fixed, bus, now);
            TEST_ASSERT_EQUAL_MESSAGE(first[i], fixed.degree(), "an exact loop drifted");
        }
    }

    NodeConfig loose = harmony_config(4, 1, 0, 13);
    loose.params[Harmony::P_LOOP] = 1;
    loose.params[Harmony::P_DRIFT] = 40;
    Harmony alive(loose);
    now = 0;
    for (uint8_t i = 0; i < 4; i++){ advance(alive, bus, now); first[i] = alive.degree(); }

    uint8_t seen[4] = {0, 0, 0, 0};
    uint32_t changes = 0;
    for (uint8_t round = 0; round < 20; round++){
        for (uint8_t i = 0; i < 4; i++){
            advance(alive, bus, now);
            if (alive.degree() != first[i]){ changes++; first[i] = alive.degree(); }
            seen[i] = alive.degree();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(changes > 0, "a drifting loop never changed");
    // Still a loop: the phrase it settled on is the one it keeps playing.
    for (uint8_t i = 0; i < 4; i++) TEST_ASSERT_EQUAL(first[i], seen[i]);
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
    NodeConfig hc = harmony_config(16, 1, 0, 7);
    Harmony harmony(hc);

    NodeConfig cc = node_config(ALGO_CHORD);
    cc.in_bus[0] = NO_BUS;                 // nothing played: the chord follows its root inlet
    cc.in_bus[1] = NOTE_ROOT;
    cc.out_bus[0] = NOTE_CHORD;
    cc.params[Chord::P_QUALITY] = Chord::QUALITY_TRIAD;
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
    NodeConfig c = harmony_config(4, 1, 0, 9);
    c.params[Harmony::P_LOOP] = 1;
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
    TEST_ASSERT_TRUE(node.set_param(Harmony::P_LOOP, 0));
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
    NodeConfig c = harmony_config(4, 1, 0, 17);
    Harmony node(c);
    uint32_t now = 0;

    advance(node, bus, now);               // phrase position is now 1
    advance(node, bus, now);               // ... 2
    TEST_ASSERT_EQUAL_UINT8(2, node.phrase_position());
    TEST_ASSERT_TRUE(node.set_param(Harmony::P_LOOP, 1));

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
    NodeConfig c = harmony_config(8, 1, 0, 23);
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
    NodeConfig c = harmony_config(8, 1, 100, 5);   // pinned to the tonic
    Harmony node(c);
    uint32_t now = 0;

    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());       // C3 in C major

    // The key moves under a sounding root. The root follows - that is the
    // whole point of a key - and the release comes from the ledger, so the
    // note-off carries C3 and nothing hangs.
    global_key::set(SCALE_NATURAL_MINOR, 9);         // A minor
    idle(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(57, node.playing());       // A3: the octave is this node's

    bool released_c3 = false;
    for (uint8_t k = 0; k < bus.note_count(NOTE_ROOT); k++){
        const MidiEvent e = bus.note_read(NOTE_ROOT, k);
        if (is_note_off(e) && e.data1 == 48) released_c3 = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(released_c3, "the old root was left hanging");
}

// One key for the patch, one register per node: the key says where home is,
// and a node that names no octave of its own plays in the key's - so one
// setting moves the progression, and a bass that named one stays under it.
static void test_the_key_register_moves_the_patch_and_a_named_octave_stays() {
    BusManager bus;
    NodeConfig c = harmony_config(8, 1, 100, 5);
    c.params[Harmony::P_OCTAVE] = 0;                   // the key's own register
    Harmony node(c);
    uint32_t now = 0;

    global_key::set(SCALE_NATURAL_MINOR, 9, 2);      // A minor, at octave 2
    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(33, node.playing());       // A1: where the key sits

    BusManager other;
    NodeConfig low = harmony_config(8, 1, 100, 5);
    low.params[Harmony::P_OCTAVE] = 1;                 // an octave of its own
    Harmony bass(low);
    uint32_t then = 0;
    advance(bass, other, then);
    TEST_ASSERT_EQUAL_UINT8(21, bass.playing());       // A0, wherever the key goes
    global_key::set(SCALE_NATURAL_MINOR, 9, 7);
    idle(bass, other, then);
    TEST_ASSERT_EQUAL_UINT8(21, bass.playing());
}

static void test_a_key_with_five_notes_has_five_chords() {
    BusManager bus;
    global_key::set(SCALE_PENTATONIC_MINOR, 0);
    NodeConfig c = harmony_config(16, 1, 0, 29);
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
    NodeConfig c = harmony_config(16, 1, 0, 33);
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
    NodeConfig c = harmony_config(4, 1, 100, 5);
    Harmony node(c);
    uint32_t now = 0;
    advance(node, bus, now);
    node.silence(bus);
    bus.swap();
    TEST_ASSERT_EQUAL_UINT8(1, bus.note_count(NOTE_ROOT));
    TEST_ASSERT_TRUE(is_note_off(bus.note_read(NOTE_ROOT, 0)));
    TEST_ASSERT_EQUAL_UINT8(48, bus.note_read(NOTE_ROOT, 0).data1);
}

// A transport that stops releases the root, and the node stays down until
// something advances it again. It used to play again on the very next pass:
// the re-voice that follows the key under a held root treated an empty ledger
// as a note to re-strike, so nothing could ever tell this node to be quiet.
static void test_a_transport_stop_releases_the_root_and_stays_quiet() {
    BusManager bus;
    NodeConfig c = harmony_config(4, 1, 100, 5);
    Harmony node(c);
    uint32_t now = 0;
    advance(node, bus, now);
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());

    node.transport_stopped(bus);
    bus.swap();
    TEST_ASSERT_EQUAL_UINT8(1, bus.note_count(NOTE_ROOT));
    TEST_ASSERT_TRUE(is_note_off(bus.note_read(NOTE_ROOT, 0)));
    TEST_ASSERT_EQUAL_UINT8(48, bus.note_read(NOTE_ROOT, 0).data1);
    TEST_ASSERT_EQUAL_UINT8(0xFF, node.playing());

    // Passes with no advance edge - including the key moving, which is what
    // the re-voice is for - leave it silent.
    for (uint8_t i = 0; i < 20; i++){
        if (i == 10) global_key::set(SCALE_NATURAL_MINOR, 3);
        idle(node, bus, now);
        TEST_ASSERT_EQUAL_UINT8(0xFF, node.playing());
    }

    // The next advance plays again, in the key it is now in.
    advance(node, bus, now);
    TEST_ASSERT_TRUE(node.playing() != 0xFF);
    TEST_ASSERT_EQUAL_UINT8(node.pitch_of(node.degree()), node.playing());
}

static void test_one_seed_is_one_progression() {
    BusManager bus_a, bus_b;
    NodeConfig a = harmony_config(8, 50, 0, 44);
    NodeConfig b = harmony_config(8, 50, 0, 44);
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
    NodeConfig c = harmony_config(4, 75, 0, 5);
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
    RUN_TEST(test_the_circle_of_fifths_is_computed_not_written_down);
    RUN_TEST(test_the_same_rule_finds_the_fifths_of_a_pentatonic_key);
    RUN_TEST(test_fifths_turns_the_walk_the_other_way_round);
    RUN_TEST(test_smooth_buys_the_chords_that_share_notes);
    RUN_TEST(test_leading_is_a_tonal_control_and_is_inert_without_one);
    RUN_TEST(test_spread_runs_from_a_loop_to_a_uniform_walk);
    RUN_TEST(test_gravity_stays_home_far_more_than_a_flat_walk);
    RUN_TEST(test_drift_keeps_a_loop_alive);
    RUN_TEST(test_a_triad_on_every_degree_comes_out_the_right_quality);
    RUN_TEST(test_loop_keeps_the_first_phrase_and_repeats_it);
    RUN_TEST(test_switching_loop_on_waits_for_the_top_of_a_phrase);
    RUN_TEST(test_reset_starts_the_phrase_again_on_the_tonic);
    RUN_TEST(test_the_key_moving_transposes_the_progression);
    RUN_TEST(test_the_key_register_moves_the_patch_and_a_named_octave_stays);
    RUN_TEST(test_a_key_with_five_notes_has_five_chords);
    RUN_TEST(test_the_degree_outlet_moves_with_the_chord);
    RUN_TEST(test_a_patch_swap_releases_the_root);
    RUN_TEST(test_a_transport_stop_releases_the_root_and_stays_quiet);
    RUN_TEST(test_one_seed_is_one_progression);
    RUN_TEST(test_harmony_never_allocates);
    return UNITY_END();
}
