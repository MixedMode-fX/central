#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_key.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "algorithm/midi/cv_to_note.h"
#include "algorithm/util/cv_to_gate.h"
#include "algorithm/modulator/lfo.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// C major unless a test says otherwise: the scale a CvToNote plays is the
// key's, so it is set here rather than on the node.
void setUp() { global_key::set(SCALE_MAJOR, 0); }
void tearDown() { global_key::set(SCALE_CHROMATIC, 0); }

// The two doors between the CV bus and the music (#30, #31). Before these the
// CV domain was closed: three modulators wrote it, the modulation matrix and
// the console read it, and nothing turned a level into a note or a gate.

// Buses used throughout: CV 0 is the signal, CV 1 velocity, gate 0 the
// trigger, gate 1 a comparator's output, note 0 the melody.
static const uint8_t CV_SIGNAL = 0, CV_VELOCITY = 1;
static const uint8_t GATE_TRIG = 0, GATE_OUT = 1;
static const uint8_t NOTE_OUT = 0;

// ---------------------------------------------------------------------------
// CvToNote
// ---------------------------------------------------------------------------

// `octave` 0 is the key's own register; the scale is the key's, always.
static NodeConfig cvn_config(uint8_t map, uint8_t octave, uint8_t range,
                             uint8_t mode, bool with_trigger){
    NodeConfig c = node_config(ALGO_CV_TO_NOTE);
    c.in_bus[0] = CV_SIGNAL;
    if (with_trigger) c.in_bus[1] = GATE_TRIG;
    c.out_bus[0] = NOTE_OUT;
    c.params[0] = map;
    c.params[1] = octave;
    c.params[2] = range;
    c.params[3] = mode;
    c.params[4] = CvToNote::CVN_UNIPOLAR;
    return c;
}

// One pass with `cv` on the signal bus and `trig` on the trigger bus.
static void cvn_pass(CvToNote& node, BusManager& bus, int16_t cv, bool trig, uint32_t now_us){
    bus.cv_write(CV_SIGNAL, cv);
    bus.gate_write(GATE_TRIG, trig);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
}

// The note-ons this pass wrote, in order.
static uint8_t collect(BusManager& bus, uint8_t* notes, uint8_t* offs){
    uint8_t n_on = 0, n_off = 0;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        const MidiEvent e = bus.note_read(NOTE_OUT, i);
        if (is_note_on(e)) notes[n_on++] = e.data1;
        else if (is_note_off(e) && offs) offs[n_off++] = e.data1;
    }
    return n_on;
}

static void test_degree_mapping_spreads_the_range_over_the_scale_evenly() {
    BusManager bus;
    // C major from C3, two octaves: fourteen degrees across full scale.
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 2,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);

    // Every degree of the scale, and only degrees of the scale.
    const uint8_t expect[14] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71};
    for (uint8_t d = 0; d < 14; d++){
        // The middle of degree d's slice of full scale.
        const int16_t cv = (int16_t)(((int32_t)d * CV_FULL) / 14 + CV_FULL / 28);
        TEST_ASSERT_EQUAL_UINT8(expect[d], node.pitch_for(cv));
    }
    // The very top of the range is the last degree, never one past it.
    TEST_ASSERT_EQUAL_UINT8(71, node.pitch_for(CV_MAX));
    TEST_ASSERT_EQUAL_UINT8(48, node.pitch_for(0));
}

static void test_snap_mapping_keeps_the_shape_of_a_melody() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_SNAP, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);

    // One octave of semitones across full scale, snapped into C major: the
    // black keys fold onto their neighbours rather than being redistributed.
    // C# (offset 1) snaps up to D, F# (6) up to G, and so on - a tie goes up,
    // which is scale_quantise's rule.
    const uint8_t expect[12] = {48, 50, 50, 52, 52, 53, 55, 55, 57, 57, 59, 59};
    for (uint8_t s = 0; s < 12; s++){
        const int16_t cv = (int16_t)(((int32_t)s * CV_FULL) / 12 + CV_FULL / 24);
        TEST_ASSERT_EQUAL_UINT8(expect[s], node.pitch_for(cv));
    }
}

static void test_a_trigger_plays_and_releases_the_previous_note() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);
    uint8_t on[8], off[8];

    cvn_pass(node, bus, 0, false, 0);
    TEST_ASSERT_EQUAL_UINT8(0, collect(bus, on, off));

    cvn_pass(node, bus, 0, true, 1000);            // the edge
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, off));
    TEST_ASSERT_EQUAL_UINT8(48, on[0]);
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());

    cvn_pass(node, bus, 0, false, 2000);           // no edge, nothing happens
    TEST_ASSERT_EQUAL_UINT8(0, collect(bus, on, off));

    // A new level and a new edge: the old note is released before the new one
    // sounds, and the release carries the pitch that was sent.
    cvn_pass(node, bus, (int16_t)(CV_FULL * 4 / 7 + 100), true, 3000);
    uint8_t n_off = 0;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        if (is_note_off(bus.note_read(NOTE_OUT, i))) { off[n_off++] = bus.note_read(NOTE_OUT, i).data1; }
    }
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(1, n_off);
    TEST_ASSERT_EQUAL_UINT8(48, off[0]);
    TEST_ASSERT_EQUAL_UINT8(55, on[0]);            // degree 4 of C major
}

static void test_a_trigger_at_the_same_pitch_still_retriggers() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);
    uint8_t on[8];

    cvn_pass(node, bus, 100, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    cvn_pass(node, bus, 100, false, 2000);
    cvn_pass(node, bus, 100, true, 3000);          // same level, same pitch
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(48, on[0]);
}

static void test_tracking_plays_one_note_per_pitch_not_per_pass() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRACK, false);
    CvToNote node(c);
    uint8_t on[8];

    // Sweep slowly through the bottom degree: one note, however many passes.
    uint8_t strikes = 0;
    for (int16_t cv = 0; cv < (int16_t)(CV_FULL / 7); cv += 8){
        cvn_pass(node, bus, cv, false, (uint32_t)cv * 100u);
        strikes += collect(bus, on, nullptr);
    }
    TEST_ASSERT_EQUAL_UINT8(1, strikes);
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());

    // Crossing into the next degree is one more note.
    cvn_pass(node, bus, (int16_t)(CV_FULL / 7 + 10), false, 900000u);
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(50, on[0]);
}

static void test_a_timed_gate_releases_on_time_and_does_not_restrike() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRACK, false);
    c.params[5] = 20;                              // 20 ms
    CvToNote node(c);
    uint8_t on[8];

    cvn_pass(node, bus, 100, false, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());

    cvn_pass(node, bus, 100, false, 10000);        // 9 ms later, still sounding
    TEST_ASSERT_EQUAL_UINT8(48, node.playing());

    cvn_pass(node, bus, 100, false, 25000);        // past the gate
    TEST_ASSERT_EQUAL_UINT8(0xFF, node.playing());

    // The level has not moved, so nothing restrikes - which is the failure a
    // "restrike whenever nothing is sounding" rule would have.
    uint8_t strikes = 0;
    for (uint32_t t = 26000; t < 500000u; t += 1000){
        cvn_pass(node, bus, 100, false, t);
        strikes += collect(bus, on, nullptr);
    }
    TEST_ASSERT_EQUAL_UINT8(0, strikes);
}

static void test_the_key_moves_the_tonic_and_keeps_the_octave() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);

    global_key::set(SCALE_NATURAL_MINOR, 9);     // A minor
    // The tonic is the key's pitch class in the register this node names:
    // A3, not A2 and not C3.
    TEST_ASSERT_EQUAL_UINT8(57, node.active_root());
    TEST_ASSERT_EQUAL_UINT8(57, node.pitch_for(0));
    TEST_ASSERT_EQUAL_UINT8(59, node.pitch_for((int16_t)(CV_FULL / 7 + 10)));  // B3

    global_key::set(SCALE_MAJOR, 0);
    TEST_ASSERT_EQUAL_UINT8(48, node.active_root());
    TEST_ASSERT_EQUAL_UINT8(50, node.pitch_for((int16_t)(CV_FULL / 7 + 10)));  // D3
}

static void test_the_key_changing_under_a_note_cannot_strand_it() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);
    uint8_t on[8], off[8];

    global_key::set(SCALE_MAJOR, 0);
    cvn_pass(node, bus, 0, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(48, on[0]);

    // The whole key moves while C3 is sounding. The release comes from the
    // ledger, so it is C3 and not the tonic of the new key.
    global_key::set(SCALE_NATURAL_MINOR, 9);
    cvn_pass(node, bus, 0, false, 2000);
    cvn_pass(node, bus, 0, true, 3000);
    uint8_t n_off = 0;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        const MidiEvent e = bus.note_read(NOTE_OUT, i);
        if (is_note_off(e)) off[n_off++] = e.data1;
    }
    TEST_ASSERT_EQUAL_UINT8(1, n_off);
    TEST_ASSERT_EQUAL_UINT8(48, off[0]);
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(57, on[0]);
}

static void test_a_patch_swap_releases_what_is_sounding() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);
    uint8_t on[8], off[8];

    cvn_pass(node, bus, 0, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, collect(bus, on, nullptr));
    node.silence(bus);
    bus.swap();
    collect(bus, on, off);
    TEST_ASSERT_EQUAL_UINT8(1, bus.note_count(NOTE_OUT));
    TEST_ASSERT_TRUE(is_note_off(bus.note_read(NOTE_OUT, 0)));
    TEST_ASSERT_EQUAL_UINT8(48, bus.note_read(NOTE_OUT, 0).data1);
}

static void test_a_velocity_inlet_outranks_the_parameter() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    c.in_bus[2] = CV_VELOCITY;
    c.params[7] = 100;
    CvToNote node(c);

    bus.cv_write(CV_SIGNAL, 0);
    bus.cv_write(CV_VELOCITY, CV_MAX);
    bus.gate_write(GATE_TRIG, true);
    bus.swap();
    node.process(bus, 1000);
    bus.swap();
    TEST_ASSERT_EQUAL_UINT8(127, bus.note_read(NOTE_OUT, 0).data2);
}

static void test_a_level_off_the_end_of_the_keyboard_is_a_rest() {
    BusManager bus;
    // Eight octaves of chromatic travel from note 120: the top of the range
    // is far past 127.
    global_key::set(SCALE_CHROMATIC, 0);
    NodeConfig c = cvn_config(CvToNote::CVN_SNAP, 10, 8,
                              CvToNote::CVN_TRIGGER, true);
    CvToNote node(c);
    uint8_t on[8];

    TEST_ASSERT_EQUAL_UINT8(0xFF, node.pitch_for(CV_MAX));
    cvn_pass(node, bus, CV_MAX, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, collect(bus, on, nullptr));
    TEST_ASSERT_EQUAL_UINT8(0xFF, node.playing());
}

static void test_bipolar_is_how_the_matrix_reads_a_signal() {
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 1,
                              CvToNote::CVN_TRIGGER, true);
    c.params[4] = CvToNote::CVN_BIPOLAR;
    CvToNote node(c);

    // A bipolar signal uses the whole range: the bottom rail is the tonic and
    // zero is the middle of it, which is what a centred Lfo needs.
    TEST_ASSERT_EQUAL_UINT8(48, node.pitch_for(-CV_HALF));
    TEST_ASSERT_EQUAL_UINT8(53, node.pitch_for(0));           // degree 3 of 7
    TEST_ASSERT_EQUAL_UINT8(59, node.pitch_for(CV_HALF - 1));
}

// ---------------------------------------------------------------------------
// CvToGate
// ---------------------------------------------------------------------------

static NodeConfig cvg_config(uint8_t threshold, uint8_t hysteresis, uint8_t mode){
    NodeConfig c = node_config(ALGO_CV_TO_GATE);
    c.in_bus[0] = CV_SIGNAL;
    c.out_bus[0] = GATE_OUT;
    c.params[0] = threshold;
    c.params[1] = hysteresis;
    c.params[2] = CvToGate::CVG_UNIPOLAR;
    c.params[3] = mode;
    return c;
}

static bool cvg_pass(CvToGate& node, BusManager& bus, int16_t cv, uint32_t now_us){
    bus.cv_write(CV_SIGNAL, cv);
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    return bus.gate_read(GATE_OUT);
}

static void test_the_comparator_raises_a_gate_at_the_threshold() {
    BusManager bus;
    NodeConfig c = cvg_config(50, 0, CvToGate::CVG_GATE);
    CvToGate node(c);

    TEST_ASSERT_FALSE(cvg_pass(node, bus, 0, 1000));
    TEST_ASSERT_FALSE(cvg_pass(node, bus, (int16_t)(CV_HALF - 1), 2000));
    TEST_ASSERT_TRUE(cvg_pass(node, bus, (int16_t)CV_HALF, 3000));
    TEST_ASSERT_TRUE(cvg_pass(node, bus, CV_MAX, 4000));
    TEST_ASSERT_FALSE(cvg_pass(node, bus, (int16_t)(CV_HALF - 1), 5000));
}

static void test_hysteresis_stops_a_signal_resting_on_the_threshold_chattering() {
    BusManager bus;
    NodeConfig c = cvg_config(50, 10, CvToGate::CVG_GATE);   // 10% of full scale
    CvToGate node(c);

    TEST_ASSERT_TRUE(cvg_pass(node, bus, (int16_t)CV_HALF, 1000));
    // Twenty passes of noise either side of the threshold: without hysteresis
    // that is twenty crossings, and on a gate bus twenty triggers.
    for (uint32_t i = 0; i < 20; i++){
        const int16_t cv = (int16_t)(CV_HALF + ((i % 2) ? -30 : 30));
        TEST_ASSERT_TRUE(cvg_pass(node, bus, cv, 2000 + i * 1000));
    }
    TEST_ASSERT_EQUAL_UINT32(1, node.crossings());
    // It falls only once the signal really has left.
    TEST_ASSERT_FALSE(cvg_pass(node, bus, (int16_t)(CV_HALF - CV_FULL / 10 - 1), 30000));
}

static void test_trigger_mode_is_a_pulse_and_not_a_level() {
    BusManager bus;
    NodeConfig c = cvg_config(50, 0, CvToGate::CVG_TRIGGER);
    c.params[4] = 5;                                          // 5 ms
    CvToGate node(c);

    TEST_ASSERT_FALSE(cvg_pass(node, bus, 0, 0));
    TEST_ASSERT_TRUE(cvg_pass(node, bus, CV_MAX, 1000));       // the crossing
    TEST_ASSERT_TRUE(cvg_pass(node, bus, CV_MAX, 4000));       // still inside 5 ms
    TEST_ASSERT_FALSE(cvg_pass(node, bus, CV_MAX, 7000));      // the level held, the pulse did not
    TEST_ASSERT_EQUAL_UINT32(1, node.crossings());
}

static void test_invert_reverses_the_comparison_not_the_wire() {
    BusManager bus;
    NodeConfig c = cvg_config(50, 10, CvToGate::CVG_GATE);
    c.params[5] = 1;
    CvToGate node(c);

    TEST_ASSERT_TRUE(cvg_pass(node, bus, 0, 1000));            // below: inverted, so high
    TEST_ASSERT_FALSE(cvg_pass(node, bus, CV_MAX, 2000));
    // The hysteresis still belongs to the comparison, so it is the *rise* of
    // the signal that is latched and the fall that has to mean it.
    TEST_ASSERT_FALSE(cvg_pass(node, bus, (int16_t)(CV_HALF + 10), 3000));
    TEST_ASSERT_TRUE(cvg_pass(node, bus, (int16_t)(CV_HALF - CV_FULL / 10 - 1), 4000));
}

// The patch the whole pair exists for: a modulator, a comparator for time and
// a quantiser for pitch, and not one note or gate sequencer between them.
static void test_an_lfo_alone_becomes_a_melody_and_a_rhythm() {
    BusManager bus;

    NodeConfig lc = node_config(ALGO_LFO);
    lc.out_bus[0] = CV_SIGNAL;
    lc.params[0] = Lfo::LFO_TRIANGLE;
    lc.params[1] = Lfo::LFO_FREE;
    lc.params[2] = 20;                       // 2 Hz
    lc.params[8] = Lfo::LFO_UNIPOLAR;
    Lfo lfo(lc);

    NodeConfig gc = cvg_config(50, 5, CvToGate::CVG_TRIGGER);
    CvToGate comparator(gc);

    global_key::set(SCALE_PENTATONIC_MINOR, 0);
    NodeConfig nc = cvn_config(CvToNote::CVN_DEGREE, 4, 2,
                               CvToNote::CVN_TRIGGER, true);
    nc.in_bus[1] = GATE_OUT;                 // the comparator plays the quantiser
    CvToNote quantiser(nc);

    uint8_t notes = 0;
    uint8_t distinct = 0;
    uint8_t seen[128] = {0};
    for (uint32_t t = 0; t < 4000000u; t += 1000){
        lfo.process(bus, t);
        comparator.process(bus, t);
        quantiser.process(bus, t);
        bus.swap();
        for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
            const MidiEvent e = bus.note_read(NOTE_OUT, i);
            if (!is_note_on(e)) continue;
            notes++;
            if (!seen[e.data1]){ seen[e.data1] = 1; distinct++; }
        }
    }
    // Four seconds of a 2 Hz triangle is eight crossings, so eight notes -
    // and they are not all the same note, because the level at the crossing
    // is where the quantiser reads.
    TEST_ASSERT_EQUAL_UINT8(8, notes);
    TEST_ASSERT_TRUE(distinct >= 1);
    // Every note is in the pentatonic minor scale on C.
    const uint16_t mask = scale_mask(SCALE_PENTATONIC_MINOR);
    for (uint8_t p = 0; p < 128; p++){
        if (!seen[p]) continue;
        TEST_ASSERT_TRUE_MESSAGE(mask & (uint16_t)(1u << (uint8_t)(((p - 48) % 12 + 12) % 12)), "off-scale note");
    }
}

static void test_the_cv_bridge_never_allocates() {
    const size_t before = g_allocations;
    BusManager bus;
    NodeConfig c = cvn_config(CvToNote::CVN_DEGREE, 4, 2,
                              CvToNote::CVN_TRACK, false);
    CvToNote node(c);
    NodeConfig gc = cvg_config(50, 5, CvToGate::CVG_TRIGGER);
    CvToGate gate(gc);
    for (uint32_t t = 0; t < 100000u; t += 1000){
        bus.cv_write(CV_SIGNAL, (int16_t)(t % CV_FULL));
        bus.swap();
        node.process(bus, t);
        gate.process(bus, t);
        bus.swap();
    }
    TEST_ASSERT_EQUAL_size_t(before, g_allocations);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_degree_mapping_spreads_the_range_over_the_scale_evenly);
    RUN_TEST(test_snap_mapping_keeps_the_shape_of_a_melody);
    RUN_TEST(test_a_trigger_plays_and_releases_the_previous_note);
    RUN_TEST(test_a_trigger_at_the_same_pitch_still_retriggers);
    RUN_TEST(test_tracking_plays_one_note_per_pitch_not_per_pass);
    RUN_TEST(test_a_timed_gate_releases_on_time_and_does_not_restrike);
    RUN_TEST(test_the_key_moves_the_tonic_and_keeps_the_octave);
    RUN_TEST(test_the_key_changing_under_a_note_cannot_strand_it);
    RUN_TEST(test_a_patch_swap_releases_what_is_sounding);
    RUN_TEST(test_a_velocity_inlet_outranks_the_parameter);
    RUN_TEST(test_a_level_off_the_end_of_the_keyboard_is_a_rest);
    RUN_TEST(test_bipolar_is_how_the_matrix_reads_a_signal);

    RUN_TEST(test_the_comparator_raises_a_gate_at_the_threshold);
    RUN_TEST(test_hysteresis_stops_a_signal_resting_on_the_threshold_chattering);
    RUN_TEST(test_trigger_mode_is_a_pulse_and_not_a_level);
    RUN_TEST(test_invert_reverses_the_comparison_not_the_wire);

    RUN_TEST(test_an_lfo_alone_becomes_a_melody_and_a_rhythm);
    RUN_TEST(test_the_cv_bridge_never_allocates);
    return UNITY_END();
}
