#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "algorithm/midi/tonnetz.h"
#include "algorithm/midi/voicer.h"

void setUp() { global_scale::set(SCALE_MAJOR, 0); }
void tearDown() { global_scale::set(SCALE_CHROMATIC, 0); }

// Tonnetz: chromatic triads where one voice moves a semitone.

static const uint8_t GATE_ADVANCE = 0, GATE_RESET = 1;
static const uint8_t NOTE_TRIAD = 0, NOTE_VOICED = 1;

static NodeConfig tonnetz_config(uint8_t cycle, uint8_t deviation = 0, uint8_t seed = 9){
    NodeConfig c = node_config(ALGO_TONNETZ);
    c.in_bus[0] = GATE_ADVANCE;
    c.in_bus[1] = GATE_RESET;
    c.out_bus[0] = NOTE_TRIAD;
    c.params[Tonnetz::P_CYCLE] = cycle;
    c.params[Tonnetz::P_DEVIATION] = deviation;
    c.params[Tonnetz::P_ROOT] = 60;                    // middle C
    c.params[Tonnetz::P_SEED] = seed;
    return c;
}

static void idle(Node& node, BusManager& bus){
    bus.gate_write(GATE_ADVANCE, false);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
}

static std::vector<MidiEvent> advance(Node& node, BusManager& bus){
    idle(node, bus);
    bus.gate_write(GATE_ADVANCE, true);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    std::vector<MidiEvent> out;
    const uint8_t n = bus.note_count(NOTE_TRIAD);
    for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(NOTE_TRIAD, i));
    return out;
}

static std::vector<uint8_t> notes_on(const std::vector<MidiEvent>& events){
    std::vector<uint8_t> out;
    for (const MidiEvent& e : events) if (is_note_on(e)) out.push_back(e.data1);
    return out;
}

// ---------------------------------------------------------------------------
// The three transforms, as arithmetic
// ---------------------------------------------------------------------------

// The definitions in the header, checked without a bus: every transform
// changes the quality, because a major triad has no major neighbour here.
static void test_the_three_transforms_are_what_they_say() {
    uint8_t root = 0; bool minor = false;              // C major

    Tonnetz::apply(Tonnetz::TRANSFORM_P, root, minor);
    TEST_ASSERT_EQUAL(0, root); TEST_ASSERT_TRUE(minor);          // C minor
    Tonnetz::apply(Tonnetz::TRANSFORM_P, root, minor);
    TEST_ASSERT_EQUAL(0, root); TEST_ASSERT_FALSE(minor);         // and back

    Tonnetz::apply(Tonnetz::TRANSFORM_L, root, minor);
    TEST_ASSERT_EQUAL(4, root); TEST_ASSERT_TRUE(minor);          // E minor
    Tonnetz::apply(Tonnetz::TRANSFORM_L, root, minor);
    TEST_ASSERT_EQUAL(0, root); TEST_ASSERT_FALSE(minor);         // and back

    Tonnetz::apply(Tonnetz::TRANSFORM_R, root, minor);
    TEST_ASSERT_EQUAL(9, root); TEST_ASSERT_TRUE(minor);          // A minor
    Tonnetz::apply(Tonnetz::TRANSFORM_R, root, minor);
    TEST_ASSERT_EQUAL(0, root); TEST_ASSERT_FALSE(minor);         // and back
}

// Every transform moves exactly one of the three notes, and by no more than a
// tone. That is the whole reason this walk sounds smooth however far from the
// key it goes.
static void test_every_transform_moves_one_voice_by_a_tone_at_most() {
    for (uint8_t t = 0; t < 3; t++){
        for (uint8_t r = 0; r < 12; r++){
            for (uint8_t q = 0; q < 2; q++){
                uint8_t root = r; bool minor = q != 0;
                const uint16_t before = (uint16_t)((1u << root)
                                      | (1u << ((root + (minor ? 3u : 4u)) % 12u))
                                      | (1u << ((root + 7u) % 12u)));
                Tonnetz::apply(t, root, minor);
                const uint16_t after = (uint16_t)((1u << root)
                                     | (1u << ((root + (minor ? 3u : 4u)) % 12u))
                                     | (1u << ((root + 7u) % 12u)));
                // Two of the three pitch classes are common to both triads.
                uint8_t shared = 0;
                for (uint8_t i = 0; i < 12; i++){
                    if ((before & (1u << i)) && (after & (1u << i))) shared++;
                }
                TEST_ASSERT_EQUAL(2, shared);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The cycles
// ---------------------------------------------------------------------------

// The header's claim: L then R traces fifths, and the majors it passes are a
// fifth apart. It is also why this node and Harmony are the same idea.
static void test_the_lr_cycle_traces_fifths() {
    BusManager bus;
    NodeConfig c = tonnetz_config(Tonnetz::TONNETZ_LR);
    Tonnetz node(c);

    advance(node, bus);                                 // C major, the start
    TEST_ASSERT_EQUAL(0, node.triad_root());
    TEST_ASSERT_FALSE(node.triad_is_minor());

    static const uint8_t WANT_ROOT[4] = {4, 7, 11, 2};  // Em G Bm D
    static const bool WANT_MINOR[4] = {true, false, true, false};
    for (uint8_t i = 0; i < 4; i++){
        advance(node, bus);
        TEST_ASSERT_EQUAL(WANT_ROOT[i], node.triad_root());
        TEST_ASSERT_EQUAL(WANT_MINOR[i], node.triad_is_minor());
    }
}

// P then L divides the octave into major thirds and comes home after six
// steps; P then R divides it into minor thirds and comes home after eight.
static void test_the_pl_and_pr_cycles_close_where_the_header_says() {
    struct Case { uint8_t cycle; uint8_t length; };
    static const Case CASES[2] = {{Tonnetz::TONNETZ_PL, 6}, {Tonnetz::TONNETZ_PR, 8}};

    for (const Case& k : CASES){
        BusManager bus;
        NodeConfig c = tonnetz_config(k.cycle);
        Tonnetz node(c);

        advance(node, bus);
        TEST_ASSERT_EQUAL(0, node.triad_root());
        TEST_ASSERT_FALSE(node.triad_is_minor());

        for (uint8_t i = 0; i < k.length; i++){
            advance(node, bus);
            if (i + 1 < k.length){
                const bool home = node.triad_root() == 0 && !node.triad_is_minor();
                TEST_ASSERT_FALSE(home);               // not before it should be
            }
        }
        TEST_ASSERT_EQUAL(0, node.triad_root());
        TEST_ASSERT_FALSE(node.triad_is_minor());
    }
}

// At zero deviation the cycle repeats exactly; a few percent turns it into a
// walk without turning it into noise.
static void test_deviation_is_what_turns_a_cycle_into_a_walk() {
    BusManager bus_exact;
    NodeConfig c = tonnetz_config(Tonnetz::TONNETZ_PL, 0);
    Tonnetz exact(c);
    std::vector<uint8_t> first;
    for (uint8_t i = 0; i < 6; i++){ advance(exact, bus_exact); first.push_back(exact.triad_root()); }
    for (uint8_t i = 0; i < 6; i++){
        advance(exact, bus_exact);
        TEST_ASSERT_EQUAL(first[i], exact.triad_root());
    }

    BusManager bus_loose;
    NodeConfig d = tonnetz_config(Tonnetz::TONNETZ_PL, 60);
    Tonnetz loose(d);
    uint8_t differences = 0;
    std::vector<uint8_t> walk;
    for (uint8_t i = 0; i < 12; i++){ advance(loose, bus_loose); walk.push_back(loose.triad_root()); }
    for (uint8_t i = 0; i < 6; i++) if (walk[i] != walk[i + 6]) differences++;
    TEST_ASSERT_TRUE(differences > 0);
}

// `diatonic` refuses any triad the key does not hold. P is never diatonic, so
// in practice it leaves the walk with L and R - and every chord it plays is
// one of the key's own.
static void test_diatonic_keeps_every_triad_inside_the_key() {
    BusManager bus;
    NodeConfig c = tonnetz_config(Tonnetz::TONNETZ_PL, 50);
    c.params[Tonnetz::P_DIATONIC] = 1;
    Tonnetz node(c);
    global_scale::set(SCALE_MAJOR, 0);

    const uint16_t mask = scale_mask(SCALE_MAJOR);
    for (uint8_t i = 0; i < 40; i++){
        const std::vector<uint8_t> played = notes_on(advance(node, bus));
        TEST_ASSERT_EQUAL(3, played.size());
        for (uint8_t n : played){
            TEST_ASSERT_TRUE(mask & (uint16_t)(1u << (n % 12u)));
        }
    }
}

// ---------------------------------------------------------------------------
// The node
// ---------------------------------------------------------------------------

// Before the first advance nothing is sounding, and reset means what it means
// everywhere else here: the next advance starts over.
static void test_nothing_sounds_before_the_first_advance_and_reset_starts_over() {
    BusManager bus;
    NodeConfig c = tonnetz_config(Tonnetz::TONNETZ_LR);
    Tonnetz node(c);

    for (uint8_t i = 0; i < 5; i++) idle(node, bus);
    TEST_ASSERT_EQUAL(0xFF, node.triad_root());
    TEST_ASSERT_EQUAL(0, node.sounding_count());

    advance(node, bus);
    advance(node, bus);
    TEST_ASSERT_EQUAL(4, node.triad_root());

    bus.gate_write(GATE_RESET, true);
    bus.swap(); node.process(bus, 0); bus.swap();
    bus.gate_write(GATE_RESET, false);
    advance(node, bus);
    TEST_ASSERT_EQUAL(0, node.triad_root());
    TEST_ASSERT_FALSE(node.triad_is_minor());
}

// The register stays where `root` put it: the walk moves a pitch class, not
// an octave, so a chromatic walk cannot drift off the keyboard.
static void test_the_triad_stays_in_the_register_it_was_given() {
    BusManager bus;
    NodeConfig c = tonnetz_config(Tonnetz::TONNETZ_FREE, 0);
    Tonnetz node(c);

    for (uint8_t i = 0; i < 60; i++){
        const std::vector<uint8_t> played = notes_on(advance(node, bus));
        TEST_ASSERT_EQUAL(3, played.size());
        for (uint8_t n : played){
            TEST_ASSERT_TRUE(n >= 54 && n <= 73);      // one triad's span around middle C
        }
    }
}

// One chord replaces another: three off and three on, every time.
static void test_tonnetz_hangs_nothing() {
    static const uint8_t CYCLES[4] = {Tonnetz::TONNETZ_LR, Tonnetz::TONNETZ_PL,
                                      Tonnetz::TONNETZ_PR, Tonnetz::TONNETZ_FREE};
    for (uint8_t k = 0; k < 4; k++){
        BusManager bus;
        NodeConfig c = tonnetz_config(CYCLES[k], 30);
        Tonnetz node(c);
        int8_t balance[128] = {0};
        bool negative = false;

        for (uint8_t i = 0; i < 50; i++){
            for (const MidiEvent& e : advance(node, bus)){
                if (is_note_on(e)) balance[e.data1]++;
                else if (is_note_off(e)){
                    balance[e.data1]--;
                    if (balance[e.data1] < 0) negative = true;
                }
            }
            TEST_ASSERT_FALSE(negative);
            TEST_ASSERT_EQUAL(3, node.sounding_count());
        }

        node.silence(bus);
        bus.swap();
        const uint8_t n = bus.note_count(NOTE_TRIAD);
        for (uint8_t i = 0; i < n; i++){
            const MidiEvent e = bus.note_read(NOTE_TRIAD, i);
            if (is_note_off(e)) balance[e.data1]--;
        }
        TEST_ASSERT_EQUAL(0, node.refused());
        for (uint8_t i = 0; i < 128; i++) TEST_ASSERT_EQUAL(0, balance[i]);
    }
}

// The composition the header argues for: this node emits root position and
// Voicer supplies the voice leading, so the two notes every transform keeps
// are held rather than re-struck. Two of three voices stay put, every step.
static void test_into_a_voicer_two_of_three_voices_are_held() {
    BusManager bus;
    NodeConfig tc = tonnetz_config(Tonnetz::TONNETZ_LR);
    tc.out_bus[0] = NOTE_TRIAD;
    Tonnetz walker(tc);

    NodeConfig vc = node_config(ALGO_VOICER);
    vc.in_bus[0] = NOTE_TRIAD;
    vc.out_bus[0] = NOTE_VOICED;
    vc.params[Voicer::P_MODE] = Voicer::VOICE_CLOSEST;
    vc.params[Voicer::P_LOW] = 48;
    vc.params[Voicer::P_HIGH] = 84;
    Voicer voicer(vc);

    // One swap per pass: the voicer is always one pass behind the walker.
    auto pass = [&](bool gate){
        bus.gate_write(GATE_ADVANCE, gate);
        bus.swap();
        walker.process(bus, 0);
        voicer.process(bus, 0);
    };

    pass(true); pass(false);                            // the first triad, then voiced
    TEST_ASSERT_EQUAL(3, voicer.voice_count());

    for (uint8_t step = 0; step < 8; step++){
        uint8_t before[3] = {voicer.voice(0), voicer.voice(1), voicer.voice(2)};
        pass(true);
        pass(false);
        uint8_t shared = 0;
        for (uint8_t i = 0; i < 3; i++){
            for (uint8_t j = 0; j < 3; j++) if (voicer.voice(i) == before[j]) shared++;
        }
        TEST_ASSERT_EQUAL(2, shared);
    }
}

int main(int, char**){
    UNITY_BEGIN();
    RUN_TEST(test_the_three_transforms_are_what_they_say);
    RUN_TEST(test_every_transform_moves_one_voice_by_a_tone_at_most);
    RUN_TEST(test_the_lr_cycle_traces_fifths);
    RUN_TEST(test_the_pl_and_pr_cycles_close_where_the_header_says);
    RUN_TEST(test_deviation_is_what_turns_a_cycle_into_a_walk);
    RUN_TEST(test_diatonic_keeps_every_triad_inside_the_key);
    RUN_TEST(test_nothing_sounds_before_the_first_advance_and_reset_starts_over);
    RUN_TEST(test_the_triad_stays_in_the_register_it_was_given);
    RUN_TEST(test_tonnetz_hangs_nothing);
    RUN_TEST(test_into_a_voicer_two_of_three_voices_are_held);
    return UNITY_END();
}
