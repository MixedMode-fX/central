#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_key.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "algorithm/midi/mirror.h"

void setUp() {}
void tearDown() { global_key::set(SCALE_CHROMATIC, 0); }

// Mirror: negative harmony, which is the circle of fifths reflected.

static const uint8_t IN_BUS = 0, ROOT_BUS = 2, OUT_BUS = 1;

static NodeConfig mirror_config(uint8_t mode, bool with_root_inlet = false){
    NodeConfig c = node_config(ALGO_MIRROR);
    c.in_bus[0] = IN_BUS;
    if (with_root_inlet) c.in_bus[1] = ROOT_BUS;
    c.out_bus[0] = OUT_BUS;
    c.params[Mirror::P_MODE] = mode;
    c.params[Mirror::P_SEED] = 7;                  // exact, so a percentage is testable
    return c;
}

static MidiEvent on(uint8_t note, uint8_t velocity = 100, uint8_t channel = 1){
    return MidiEvent{MIDI_NOTE_ON, channel, note, velocity};
}
static MidiEvent off(uint8_t note, uint8_t channel = 1){
    return MidiEvent{MIDI_NOTE_OFF, channel, note, 0};
}

static std::vector<MidiEvent> run_pass(BusManager& bus, Node& node){
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    std::vector<MidiEvent> out;
    const uint8_t n = bus.note_count(OUT_BUS);
    for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(OUT_BUS, i));
    return out;
}

static std::vector<uint8_t> classes_of(const std::vector<MidiEvent>& events){
    std::vector<uint8_t> out;
    for (const MidiEvent& e : events) if (is_note_on(e)) out.push_back((uint8_t)(e.data1 % 12u));
    return out;
}

static bool has_class(const std::vector<uint8_t>& classes, uint8_t pitch_class){
    for (uint8_t c : classes) if (c == pitch_class) return true;
    return false;
}

// The table in the header, checked chord by chord. The tonic becomes the
// tonic minor, the dominant becomes the subdominant minor, and the
// subdominant becomes the minor dominant - which is the reflection exchanging
// the two halves of the circle about the tonic.
static void test_negative_harmony_maps_the_three_chords() {
    global_key::set(SCALE_MAJOR, 0);                 // C major

    struct Case { uint8_t in[3]; uint8_t want[3]; };
    static const Case CASES[3] = {
        {{60, 64, 67}, {0, 3, 7}},                     // I    -> i     C E flat G
        {{67, 71, 74}, {0, 5, 8}},                     // V    -> iv    F A flat C
        {{65, 69, 72}, {2, 7, 10}},                    // IV   -> v     G B flat D
    };

    for (const Case& c : CASES){
        BusManager bus;
        NodeConfig config = mirror_config(Mirror::MIRROR_NEGATIVE);
        Mirror node(config);
        for (uint8_t n : c.in) bus.note_write(IN_BUS, on(n));
        const std::vector<uint8_t> got = classes_of(run_pass(bus, node));
        TEST_ASSERT_EQUAL(3, got.size());
        for (uint8_t want : c.want) TEST_ASSERT_TRUE(has_class(got, want));
    }
}

// Reflecting the pitch rather than the pitch class puts middle C somewhere
// off the bottom of the keyboard. The reflection is a pitch class, placed in
// the octave nearest the note that caused it.
static void test_the_reflection_lands_near_the_note_that_caused_it() {
    global_key::set(SCALE_MAJOR, 0);
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE);
    Mirror node(c);

    static const uint8_t NOTES[5] = {36, 48, 60, 72, 96};
    for (uint8_t n : NOTES){
        const uint8_t got = node.reflect(n);
        TEST_ASSERT_NOT_EQUAL(0xFF, got);
        const int16_t distance = (int16_t)got - (int16_t)n;
        TEST_ASSERT_TRUE(distance >= -6 && distance <= 6);
    }
}

// Inversion is the same operation about the tonic itself: what a melody wants
// rather than what a progression does. In C, E goes to A flat and G to F.
static void test_inversion_reflects_about_the_tonic() {
    global_key::set(SCALE_MAJOR, 0);
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_INVERSION);
    Mirror node(c);

    TEST_ASSERT_EQUAL(0, node.reflect(60) % 12);       // C is its own reflection
    TEST_ASSERT_EQUAL(8, node.reflect(64) % 12);       // E  -> A flat
    TEST_ASSERT_EQUAL(5, node.reflect(67) % 12);       // G  -> F
}

// The axis is the key's, and a cable outranks the key - the rule every node
// with a root inlet follows.
static void test_the_root_inlet_moves_the_axis() {
    global_key::set(SCALE_MAJOR, 0);
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE, true);
    Mirror node(c);

    bus.note_write(ROOT_BUS, on(62));                  // the axis is D's now
    bus.note_write(IN_BUS, on(62));
    const std::vector<uint8_t> got = classes_of(run_pass(bus, node));
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL(9, got[0]);                      // D -> A, the tonic to its dominant
    TEST_ASSERT_EQUAL(2, node.active_root());
}

// A reflection that stayed in the key would be a transposition, so `snap` is
// off by default. Turned on, the reflection comes back into the scale: in C
// major, E flat is not there and D is the nearest tone that is.
static void test_snap_puts_the_reflection_back_in_the_key() {
    global_key::set(SCALE_MAJOR, 0);
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE);
    Mirror node(c);

    TEST_ASSERT_EQUAL(3, node.reflect(64) % 12);       // E flat: outside C major
    TEST_ASSERT_TRUE(node.set_param(Mirror::P_SNAP, 1));
    const uint8_t snapped = node.reflect(64) % 12;
    TEST_ASSERT_TRUE(snapped == 2 || snapped == 4);    // into the scale, either way the tie fell
    TEST_ASSERT_TRUE(scale_mask(SCALE_MAJOR) & (uint16_t)(1u << snapped));
}

// The default is every note reflected, because that is what somebody who
// patched a mirror asked for. At one percent almost nothing is - which is how
// "off" is spelled, since a stored zero means the default.
static void test_amount_is_a_percentage_of_the_note_ons() {
    global_key::set(SCALE_MAJOR, 0);
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE);
    Mirror node(c);
    TEST_ASSERT_EQUAL(Mirror::DEFAULT_AMOUNT, node.get_param(Mirror::P_AMOUNT));

    uint16_t reflected = 0;
    for (uint16_t i = 0; i < 200; i++){
        bus.note_write(IN_BUS, on(64));
        const std::vector<MidiEvent> out = run_pass(bus, node);
        TEST_ASSERT_EQUAL(1, out.size());
        if (out[0].data1 != 64) reflected++;
        bus.note_write(IN_BUS, off(64));
        run_pass(bus, node);
    }
    TEST_ASSERT_EQUAL(200, reflected);

    TEST_ASSERT_TRUE(node.set_param(Mirror::P_AMOUNT, 1));
    reflected = 0;
    for (uint16_t i = 0; i < 200; i++){
        bus.note_write(IN_BUS, on(64));
        const std::vector<MidiEvent> out = run_pass(bus, node);
        if (out.size() && out[0].data1 != 64) reflected++;
        bus.note_write(IN_BUS, off(64));
        run_pass(bus, node);
    }
    TEST_ASSERT_TRUE(reflected < 20);
}

// The failure this class of algorithm is prone to: the axis moves under a
// sounding note and the note-off is computed from the new one. The release is
// taken from the ledger instead, so it is the pitch that was actually sent.
static void test_moving_the_axis_under_a_sounding_note_releases_what_was_sent() {
    global_key::set(SCALE_MAJOR, 0);
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE);
    Mirror node(c);

    bus.note_write(IN_BUS, on(64));
    std::vector<MidiEvent> out = run_pass(bus, node);
    TEST_ASSERT_EQUAL(1, out.size());
    const uint8_t emitted = out[0].data1;
    TEST_ASSERT_EQUAL(1, node.sounding_count());

    global_key::set(SCALE_MAJOR, 7);                 // the whole module changes key
    TEST_ASSERT_TRUE(node.set_param(Mirror::P_MODE, Mirror::MIRROR_INVERSION));

    bus.note_write(IN_BUS, off(64));
    out = run_pass(bus, node);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_TRUE(is_note_off(out[0]));
    TEST_ASSERT_EQUAL(emitted, out[0].data1);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// Six hundred note-ons and note-offs at half strength, in a key that moves:
// every note-on paired, and never a release of something that was not sent.
static void test_mirror_hangs_nothing() {
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE);
    c.params[Mirror::P_AMOUNT] = 50;
    Mirror node(c);

    int8_t balance[128] = {0};
    bool negative = false;
    uint8_t held[8] = {0};
    uint8_t n_held = 0;
    uint32_t state = 12345;

    for (uint16_t i = 0; i < 600; i++){
        state = state * 1103515245u + 12345u;
        const bool play = (n_held == 0) || ((state >> 16) & 1u);
        if (play && n_held < 8){
            const uint8_t note = (uint8_t)(36 + ((state >> 8) % 48u));
            bool already = false;
            for (uint8_t j = 0; j < n_held; j++) if (held[j] == note) already = true;
            if (!already){ bus.note_write(IN_BUS, on(note)); held[n_held++] = note; }
        } else if (n_held){
            const uint8_t index = (uint8_t)((state >> 8) % n_held);
            bus.note_write(IN_BUS, off(held[index]));
            for (uint8_t j = index; j + 1u < n_held; j++) held[j] = held[j + 1];
            n_held--;
        }
        if ((i % 64) == 0) global_key::set(SCALE_MAJOR, (uint8_t)(i % 12u));

        for (const MidiEvent& e : run_pass(bus, node)){
            if (is_note_on(e)) balance[e.data1]++;
            else if (is_note_off(e)){ balance[e.data1]--; if (balance[e.data1] < 0) negative = true; }
        }
        TEST_ASSERT_FALSE(negative);
    }

    while (n_held){
        bus.note_write(IN_BUS, off(held[--n_held]));
        for (const MidiEvent& e : run_pass(bus, node)){
            if (is_note_on(e)) balance[e.data1]++;
            else if (is_note_off(e)){ balance[e.data1]--; if (balance[e.data1] < 0) negative = true; }
        }
    }

    TEST_ASSERT_FALSE(negative);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
    TEST_ASSERT_EQUAL(0, node.refused());
    for (uint8_t i = 0; i < 128; i++) TEST_ASSERT_EQUAL(0, balance[i]);
}

// Anything that is not a note-on or a note-off is not this node's business.
static void test_other_messages_pass_through() {
    BusManager bus;
    NodeConfig c = mirror_config(Mirror::MIRROR_NEGATIVE);
    Mirror node(c);

    bus.note_write(IN_BUS, MidiEvent{MIDI_CONTROL_CHANGE, 1, 74, 100});
    const std::vector<MidiEvent> out = run_pass(bus, node);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, out[0].type);
    TEST_ASSERT_EQUAL(74, out[0].data1);
}

int main(int, char**){
    UNITY_BEGIN();
    RUN_TEST(test_negative_harmony_maps_the_three_chords);
    RUN_TEST(test_the_reflection_lands_near_the_note_that_caused_it);
    RUN_TEST(test_inversion_reflects_about_the_tonic);
    RUN_TEST(test_the_root_inlet_moves_the_axis);
    RUN_TEST(test_snap_puts_the_reflection_back_in_the_key);
    RUN_TEST(test_amount_is_a_percentage_of_the_note_ons);
    RUN_TEST(test_moving_the_axis_under_a_sounding_note_releases_what_was_sent);
    RUN_TEST(test_mirror_hangs_nothing);
    RUN_TEST(test_other_messages_pass_through);
    return UNITY_END();
}
