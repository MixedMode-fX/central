#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "algorithm/midi/voicer.h"
#include "algorithm/midi/chord.h"

void setUp() {}
void tearDown() { global_scale::set(SCALE_CHROMATIC, 0); }

// Voicer: where a chord's notes sit, and what moves when it changes.

static const uint8_t IN_BUS = 0, OUT_BUS = 1;

static NodeConfig voicer_config(uint8_t mode, uint8_t low = 0, uint8_t high = 0){
    NodeConfig c = node_config(ALGO_VOICER);
    c.in_bus[0] = IN_BUS;
    c.out_bus[0] = OUT_BUS;
    c.params[Voicer::P_MODE] = mode;
    c.params[Voicer::P_LOW] = low;
    c.params[Voicer::P_HIGH] = high;
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

// One chord replacing another, the way `Chord` sends it: the old voices
// released and the new ones sounded in the same pass, in that order.
static std::vector<MidiEvent> play_chord(BusManager& bus, Node& node,
                                         const std::vector<uint8_t>& previous,
                                         const std::vector<uint8_t>& notes){
    for (uint8_t n : previous) bus.note_write(IN_BUS, off(n));
    for (uint8_t n : notes) bus.note_write(IN_BUS, on(n));
    return run_pass(bus, node);
}

// `silence` writes note-offs straight into the pass that is being built, so
// one swap makes them readable - a second would carry them away again.
static std::vector<MidiEvent> collect_silence(BusManager& bus, Node& node){
    node.silence(bus);
    bus.swap();
    std::vector<MidiEvent> out;
    const uint8_t n = bus.note_count(OUT_BUS);
    for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(OUT_BUS, i));
    return out;
}

static std::vector<uint8_t> voicing_of(const Voicer& node){
    std::vector<uint8_t> out;
    for (uint8_t i = 0; i < node.voice_count(); i++) out.push_back(node.voice(i));
    return out;
}

// Every note-on must be released exactly once, whatever the voicing did.
struct NoteBalance {
    int8_t sounding[128];
    bool went_negative;
    NoteBalance() : sounding(), went_negative(false) {}
    void observe(const std::vector<MidiEvent>& events){
        for (const MidiEvent& e : events){
            if (is_note_on(e)) sounding[e.data1]++;
            else if (is_note_off(e)){
                sounding[e.data1]--;
                if (sounding[e.data1] < 0) went_negative = true;
            }
        }
    }
    bool balanced() const {
        for (uint8_t i = 0; i < 128; i++) if (sounding[i] != 0) return false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

// With nothing sounding there is nothing to be near, so the first chord is
// stacked up from the bottom of the range whatever the mode is.
static void test_the_first_chord_is_stacked_from_the_low_note() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    Voicer node(c);

    std::vector<MidiEvent> out = play_chord(bus, node, {}, {72, 76, 79});   // C E G, an octave up
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(48, out[0].data1);
    TEST_ASSERT_EQUAL(52, out[1].data1);
    TEST_ASSERT_EQUAL(55, out[2].data1);
}

// The whole point, and the example the header argues from: C E G to F A C
// moves three semitones in total, because C is in both chords and stays where
// it is. In root position the same change is fifteen.
static void test_the_closest_voicing_holds_the_common_tone() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    TEST_ASSERT_EQUAL(48, node.voice(0));
    TEST_ASSERT_EQUAL(52, node.voice(1));
    TEST_ASSERT_EQUAL(55, node.voice(2));

    // F major, a fifth away, sent as `Chord` sends it.
    std::vector<MidiEvent> out = play_chord(bus, node, {60, 64, 67}, {65, 69, 72});
    const std::vector<uint8_t> after = voicing_of(node);
    TEST_ASSERT_EQUAL(3, after.size());
    TEST_ASSERT_EQUAL(48, after[0]);      // C, exactly where it was
    TEST_ASSERT_EQUAL(53, after[1]);      // E -> F, one semitone
    TEST_ASSERT_EQUAL(57, after[2]);      // G -> A, two

    // And the common tone was neither released nor struck again: two voices
    // moved, so two note-offs and two note-ons, not three and three.
    TEST_ASSERT_EQUAL(4, out.size());
    for (const MidiEvent& e : out) TEST_ASSERT_NOT_EQUAL(48, e.data1);
}

// `root` is what the chord did before this node existed, and it is in the
// list so the difference can be heard rather than argued.
static void test_root_position_stacks_every_chord_from_the_bottom() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_ROOT, 48, 84);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    play_chord(bus, node, {60, 64, 67}, {65, 69, 72});
    const std::vector<uint8_t> after = voicing_of(node);
    TEST_ASSERT_EQUAL(53, after[0]);      // F A C, stacked from `low`
    TEST_ASSERT_EQUAL(57, after[1]);
    TEST_ASSERT_EQUAL(60, after[2]);
}

// `spread` opens the chord out: at least a fifth between adjacent voices, so
// a triad covers most of three octaves.
static void test_spread_puts_a_fifth_between_the_voices() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_SPREAD, 48, 100);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    const std::vector<uint8_t> v = voicing_of(node);
    TEST_ASSERT_EQUAL(3, v.size());
    TEST_ASSERT_EQUAL(48, v[0]);
    TEST_ASSERT_EQUAL(64, v[1]);
    TEST_ASSERT_EQUAL(79, v[2]);
    for (uint8_t i = 1; i < v.size(); i++) TEST_ASSERT_TRUE(v[i] - v[i - 1] >= Voicer::SPREAD_GAP);
}

// Drop 2 is a fixed shape and deliberately not voice-led: root position, then
// the second voice from the top an octave down.
static void test_drop_two_lowers_the_second_voice_from_the_top() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_DROP2, 48, 84);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    const std::vector<uint8_t> v = voicing_of(node);
    TEST_ASSERT_EQUAL(3, v.size());
    TEST_ASSERT_EQUAL(40, v[0]);          // E, dropped an octave from 52
    TEST_ASSERT_EQUAL(48, v[1]);
    TEST_ASSERT_EQUAL(55, v[2]);
}

// The setting that makes root motion audible, and the one that costs motion.
// The header's own example: bass free is three semitones, bass pinned is
// fifteen, and both are things people want.
static void test_pinning_the_bass_costs_motion_and_is_meant_to() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    c.params[Voicer::P_BASS] = 1;
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    play_chord(bus, node, {60, 64, 67}, {65, 69, 72});
    const std::vector<uint8_t> v = voicing_of(node);
    TEST_ASSERT_EQUAL(3, v.size());
    TEST_ASSERT_EQUAL(53, v[0]);          // F is the bass, because it is the chord's
    TEST_ASSERT_TRUE(v[1] > v[0]);
    TEST_ASSERT_TRUE(v[2] > v[1]);
    TEST_ASSERT_EQUAL(5, v[0] % 12);
}

// `voices` turns a ninth into a shell without touching the chord upstream.
static void test_voices_caps_the_chord_from_its_bass_upward() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_ROOT, 48, 100);
    c.params[Voicer::P_VOICES] = 2;
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67, 71, 74});     // Cmaj9
    TEST_ASSERT_EQUAL(2, node.voice_count());
    TEST_ASSERT_EQUAL(48, node.voice(0));
    TEST_ASSERT_EQUAL(52, node.voice(1));
}

// A doubled pitch class is one voice: this node places classes, and two
// voices on one pitch is a unison nobody asked for.
static void test_a_doubled_octave_is_one_voice() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_ROOT, 48, 100);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67, 72});         // C E G with the root doubled
    TEST_ASSERT_EQUAL(3, node.voice_count());
}

// ---------------------------------------------------------------------------
// Retention, and the ledger
// ---------------------------------------------------------------------------

// The switch that turns the retention off: every voice re-struck, common tones
// included, which is what a rhythmic part wants and a pad does not.
static void test_retrigger_re_strikes_the_common_tone() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    c.params[Voicer::P_RETRIGGER] = 1;
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    std::vector<MidiEvent> out = play_chord(bus, node, {60, 64, 67}, {65, 69, 72});
    TEST_ASSERT_EQUAL(6, out.size());
    for (uint8_t i = 0; i < 3; i++) TEST_ASSERT_TRUE(is_note_off(out[i]));
    for (uint8_t i = 3; i < 6; i++) TEST_ASSERT_TRUE(is_note_on(out[i]));
}

// Nothing held is nothing sounding: lifting the chord releases every voice.
static void test_lifting_the_chord_releases_every_voice() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    TEST_ASSERT_EQUAL(3, node.sounding_count());

    std::vector<MidiEvent> out = play_chord(bus, node, {60, 64, 67}, {});
    TEST_ASSERT_EQUAL(3, out.size());
    for (const MidiEvent& e : out) TEST_ASSERT_TRUE(is_note_off(e));
    TEST_ASSERT_EQUAL(0, node.sounding_count());
    TEST_ASSERT_EQUAL(0, node.voice_count());
}

// A chord that has not changed is not re-sent: a downstream arpeggiator sees
// one chord, not a note-on every pass.
static void test_an_unchanged_chord_emits_nothing() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    Voicer node(c);

    play_chord(bus, node, {}, {60, 64, 67});
    for (uint8_t i = 0; i < 20; i++){
        TEST_ASSERT_EQUAL(0, run_pass(bus, node).size());
    }
    TEST_ASSERT_EQUAL(3, node.sounding_count());
}

// Moving the range under a sounding chord re-voices it, and the release comes
// from the ledger - so it is the pitch that was sent, never a re-placed one.
static void test_moving_the_range_re_voices_without_stranding_a_note() {
    BusManager bus;
    NodeConfig c = voicer_config(Voicer::VOICE_ROOT, 48, 84);
    Voicer node(c);
    NoteBalance balance;

    balance.observe(play_chord(bus, node, {}, {60, 64, 67}));
    TEST_ASSERT_EQUAL(48, node.voice(0));

    node.set_param(Voicer::P_LOW, 60);
    balance.observe(run_pass(bus, node));
    TEST_ASSERT_EQUAL(60, node.voice(0));

    balance.observe(collect_silence(bus, node));
    TEST_ASSERT_TRUE(balance.balanced());
    TEST_ASSERT_FALSE(balance.went_negative);
}

// Ten chord changes across four modes, every one of them balanced.
static void test_voicer_hangs_nothing() {
    static const uint8_t MODES[4] = {Voicer::VOICE_CLOSEST, Voicer::VOICE_ROOT,
                                     Voicer::VOICE_DROP2, Voicer::VOICE_SPREAD};
    static const uint8_t ROOTS[10] = {60, 65, 71, 62, 67, 60, 69, 64, 55, 60};

    for (uint8_t m = 0; m < 4; m++){
        BusManager bus;
        NodeConfig c = voicer_config(MODES[m], 40, 90);
        Voicer node(c);
        NoteBalance balance;
        std::vector<uint8_t> previous;

        for (uint8_t i = 0; i < 10; i++){
            const std::vector<uint8_t> chord = {ROOTS[i], (uint8_t)(ROOTS[i] + 4),
                                                (uint8_t)(ROOTS[i] + 7)};
            balance.observe(play_chord(bus, node, previous, chord));
            TEST_ASSERT_FALSE(balance.went_negative);
            previous = chord;
        }
        balance.observe(collect_silence(bus, node));
        TEST_ASSERT_TRUE(balance.balanced());
        TEST_ASSERT_FALSE(balance.went_negative);
        TEST_ASSERT_EQUAL(0, node.refused());
    }
}

// ---------------------------------------------------------------------------
// Downstream of Chord, which is where it actually sits
// ---------------------------------------------------------------------------

// The patch the design note ends on, minus the clock: a self-playing `Chord`
// walked by a root, voiced. `Chord` releases and re-sounds in one pass, and
// the voicer has to see one chord replacing another rather than the silence
// in between.
static void test_a_chord_walked_by_a_root_is_voice_led() {
    BusManager bus;
    global_scale::set(SCALE_MAJOR, 0);

    NodeConfig cc = node_config(ALGO_CHORD);
    cc.in_bus[0] = NO_BUS;                 // plays itself
    cc.in_bus[1] = 2;                      // root inlet
    cc.out_bus[0] = IN_BUS;
    cc.params[Chord::P_QUALITY] = Chord::QUALITY_TRIAD;
    Chord chord(cc);

    NodeConfig vc = voicer_config(Voicer::VOICE_CLOSEST, 48, 84);
    Voicer voicer(vc);

    // One swap per pass, which is what the master does: a stage's output is
    // its successor's input on the *next* pass, so the voicer is always one
    // pass behind the chord. That latency is the graph's model, not a wait.
    auto pass = [&](){ bus.swap(); chord.process(bus, 0); voicer.process(bus, 0); };
    pass();                                // the chord sounds its tonic triad
    pass();                                // the voicer places it
    TEST_ASSERT_EQUAL(3, voicer.voice_count());
    TEST_ASSERT_EQUAL(48, voicer.voice(0));

    // Walk the root to F. The chord releases three and sounds three; the
    // voicer moves two voices and holds one.
    bus.note_write(2, on(65));
    pass();
    pass();

    std::vector<uint8_t> v;
    for (uint8_t i = 0; i < voicer.voice_count(); i++) v.push_back(voicer.voice(i));
    TEST_ASSERT_EQUAL(3, v.size());
    TEST_ASSERT_EQUAL(48, v[0]);           // C held across the change
    TEST_ASSERT_EQUAL(53, v[1]);
    TEST_ASSERT_EQUAL(57, v[2]);
}

int main(int, char**){
    UNITY_BEGIN();
    RUN_TEST(test_the_first_chord_is_stacked_from_the_low_note);
    RUN_TEST(test_the_closest_voicing_holds_the_common_tone);
    RUN_TEST(test_root_position_stacks_every_chord_from_the_bottom);
    RUN_TEST(test_spread_puts_a_fifth_between_the_voices);
    RUN_TEST(test_drop_two_lowers_the_second_voice_from_the_top);
    RUN_TEST(test_pinning_the_bass_costs_motion_and_is_meant_to);
    RUN_TEST(test_voices_caps_the_chord_from_its_bass_upward);
    RUN_TEST(test_a_doubled_octave_is_one_voice);
    RUN_TEST(test_retrigger_re_strikes_the_common_tone);
    RUN_TEST(test_lifting_the_chord_releases_every_voice);
    RUN_TEST(test_an_unchanged_chord_emits_nothing);
    RUN_TEST(test_moving_the_range_re_voices_without_stranding_a_note);
    RUN_TEST(test_voicer_hangs_nothing);
    RUN_TEST(test_a_chord_walked_by_a_root_is_voice_led);
    return UNITY_END();
}
