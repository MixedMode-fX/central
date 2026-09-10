#include <unity.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "master.h"
#include "midi/scale.h"
#include "midi/global_scale.h"
#include "midi/note_event.h"
#include "algorithm/sequencer/note_sequencer.h"
#include "algorithm/sequencer/sequencers.h"

void setUp() {}
// The module's key is process-wide (midi/global_scale.h): every test gets it
// back the way it found it.
void tearDown() { global_scale::set(SCALE_CHROMATIC, 0); }

typedef NoteSequencerBase NS;

// ---------------------------------------------------------------------------
// Rig: advance edges from a fake gate source, exactly as MixedModeMaster
// runs a node. Two passes per edge, 20 ms apart, so the step period the
// sequencer measures is 40 ms.
// ---------------------------------------------------------------------------
static const uint8_t ADVANCE = 0, RESET = 1, OUT = 3, ROOT_BUS = 1;
static const uint32_t HALF_US = 20000;

struct Rig {
    BusManager bus;
    uint32_t now;
    Rig() : bus(), now(0) {}

    std::vector<MidiEvent> collect() {
        std::vector<MidiEvent> out;
        const uint8_t n = bus.note_count(OUT);
        for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(OUT, i));
        return out;
    }
    // One pass with the advance gate low, `dt` after the last one.
    std::vector<MidiEvent> pass(Node& node, uint32_t dt = HALF_US) {
        now += dt;
        bus.swap(); node.process(bus, now); bus.swap();
        return collect();
    }
    // One advance edge: a pass with the gate high, then one with it low.
    std::vector<MidiEvent> edge(Node& node) {
        bus.gate_write(ADVANCE, true);
        bus.swap(); node.process(bus, now); bus.swap();
        std::vector<MidiEvent> out = collect();
        now += HALF_US;
        std::vector<MidiEvent> more = pass(node);
        out.insert(out.end(), more.begin(), more.end());
        return out;
    }
    void reset_pulse(Node& node) {
        bus.gate_write(RESET, true);
        bus.swap(); node.process(bus, now); bus.swap();
        pass(node);
    }
};

static std::string describe(const std::vector<MidiEvent>& events) {
    std::string s;
    for (const MidiEvent& e : events) {
        char buf[24];
        snprintf(buf, sizeof buf, "%s%u/%u ", is_note_on(e) ? "+" : (is_note_off(e) ? "-" : "?"), e.data1, e.data2);
        s += buf;
    }
    return s;
}

static void expect(const char* expected, const std::vector<MidiEvent>& events) {
    TEST_ASSERT_EQUAL_STRING(expected, describe(events).c_str());
}

static NodeConfig seq_config(uint8_t id, uint8_t length, uint16_t scale = scale_mask(SCALE_MAJOR), uint8_t root = 60) {
    NodeConfig c = node_config(id);
    c.in_bus[0] = ADVANCE;
    c.out_bus[0] = OUT;
    c.params[NS::P_LENGTH] = length;
    c.params[NS::P_SCALE_LO] = (uint8_t)(scale & 0xFF);
    c.params[NS::P_SCALE_HI] = (uint8_t)(scale >> 8);
    c.params[NS::P_ROOT] = root;
    return c;
}

static void set_step(NodeConfig& c, uint8_t voices, uint8_t step, uint8_t voice, int8_t degree, uint8_t velocity,
                     uint8_t length = 1, uint8_t flags = 0, uint8_t probability = 0) {
    uint8_t* b = &c.params[NS::STEP_BASE + step * NS::stride(voices)];
    b[voice * 2] = (uint8_t)degree;
    b[voice * 2 + 1] = velocity;
    b[voices * 2] = (uint8_t)((length & NS::LENGTH_MASK) | flags);
    b[voices * 2 + 1] = probability;
}

// A scale run, one degree per step at velocity 100.
static NodeConfig scale_run(uint8_t length, uint16_t scale = scale_mask(SCALE_MAJOR), uint8_t root = 60) {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, length, scale, root);
    for (uint8_t i = 0; i < length; i++) set_step(c, 1, i, 0, (int8_t)i, 100);
    return c;
}

// ---------------------------------------------------------------------------
// Degrees, roots and scales
// ---------------------------------------------------------------------------

static void test_degree_wrapping_in_7_and_5_note_scales() {
    const uint16_t major = scale_mask(SCALE_MAJOR);               // 0 2 4 5 7 9 11
    const int16_t expected7[] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16};
    for (int16_t d = 0; d < 10; d++) TEST_ASSERT_EQUAL_INT16(expected7[d], scale_degree_to_semitone(d, major));
    TEST_ASSERT_EQUAL_INT16(-1, scale_degree_to_semitone(-1, major));    // the seventh below
    TEST_ASSERT_EQUAL_INT16(-3, scale_degree_to_semitone(-2, major));
    TEST_ASSERT_EQUAL_INT16(-12, scale_degree_to_semitone(-7, major));
    TEST_ASSERT_EQUAL_INT16(-13, scale_degree_to_semitone(-8, major));

    const uint16_t penta = scale_mask(SCALE_PENTATONIC_MAJOR);    // 0 2 4 7 9
    const int16_t expected5[] = {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24};
    for (int16_t d = 0; d < 11; d++) TEST_ASSERT_EQUAL_INT16(expected5[d], scale_degree_to_semitone(d, penta));
    TEST_ASSERT_EQUAL_INT16(-3, scale_degree_to_semitone(-1, penta));
    TEST_ASSERT_EQUAL_INT16(-12, scale_degree_to_semitone(-5, penta));

    TEST_ASSERT_EQUAL(12, scale_size(0));                          // empty is chromatic
    TEST_ASSERT_EQUAL_INT16(13, scale_degree_to_semitone(13, 0));
    TEST_ASSERT_EQUAL(7, scale_size(major));
    TEST_ASSERT_EQUAL(5, scale_size(penta));
}

// A mono sequence of known degrees against a known root and scale emits
// exactly the expected pitches, over two full cycles, and every note-on is
// followed by its own note-off on the next edge.
static void test_mono_sequence_emits_the_expected_pitches_twice_over() {
    NoteSequencer node(scale_run(8));
    Rig rig;
    const uint8_t pitches[8] = {60, 62, 64, 65, 67, 69, 71, 72};
    uint8_t previous = 0xFF;
    for (uint8_t i = 0; i < 16; i++) {
        const std::vector<MidiEvent> ev = rig.edge(node);
        char expected[32];
        if (previous == 0xFF) snprintf(expected, sizeof expected, "+%u/100 ", pitches[i % 8]);
        else snprintf(expected, sizeof expected, "-%u/0 +%u/100 ", previous, pitches[i % 8]);
        expect(expected, ev);
        previous = pitches[i % 8];
    }
    TEST_ASSERT_EQUAL(1, node.sounding_count());
}

// Changing the root mid-sequence transposes subsequent notes and leaves the
// sounding note correctly released: the note-off matches the note-on that
// was sent, not the new root.
static void test_root_change_releases_what_was_sent_and_transposes_the_rest() {
    NoteSequencer node(scale_run(4));
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 +62/100 ", rig.edge(node));
    node.set_root(67);                                          // G, while 62 sounds
    expect("-62/0 +71/100 ", rig.edge(node));                   // degree 2 of G major, not 69
    expect("-71/0 +72/100 ", rig.edge(node));
    node.set_root(48);
    expect("-72/0 +48/100 ", rig.edge(node));
}

// The same, with the root arriving on a note bus: playing a key transposes
// the running sequence, and only note-ons count.
// A sequencer's root names an octave and a pitch class cannot, which is why
// it used to be the exception to the key's root half. A key with a register
// can name one, so the exception is now only for the case that still cannot:
// with no register the pattern keeps its anchor exactly as it always has.
static void test_a_pattern_keeps_its_anchor_until_the_key_names_a_register() {
    NodeConfig c = scale_run(4, 0);                    // no scale of its own: follows
    NoteSequencer node(c);
    Rig rig;

    global_scale::set(SCALE_MAJOR, 9);                 // A major, and no register
    expect("+60/100 ", rig.edge(node));                // still C4, as it always was
    TEST_ASSERT_EQUAL(60, node.active_root());

    global_scale::set(SCALE_MAJOR, 9, 3);              // now the key has one: A2
    expect("-60/0 +47/100 ", rig.edge(node));          // step 1: degree 1 of A2
    TEST_ASSERT_EQUAL(45, node.active_root());
    TEST_ASSERT_EQUAL(60, node.root_note());           // the stored anchor is untouched

    // A pattern that named its own scale is not moved by the key at all.
    NodeConfig own = scale_run(4, scale_mask(SCALE_MAJOR));
    NoteSequencer fixed(own);
    Rig other;
    expect("+60/100 ", other.edge(fixed));
    TEST_ASSERT_EQUAL(60, fixed.active_root());
}

// A cable is the most explicit thing a user can say, so a patched root inlet
// outranks the key's register as it outranks everything else.
static void test_the_root_inlet_outranks_the_key_register() {
    NodeConfig c = scale_run(4, 0);
    c.in_bus[2] = ROOT_BUS;
    NoteSequencer node(c);
    Rig rig;

    global_scale::set(SCALE_MAJOR, 9, 3);
    expect("+60/100 ", rig.edge(node));                // the inlet is patched: its own
    TEST_ASSERT_EQUAL(60, node.active_root());

    rig.bus.note_write(ROOT_BUS, MidiEvent{MIDI_NOTE_ON, 1, 55, 90});
    expect("-60/0 +57/100 ", rig.edge(node));          // step 1: degree 1 of G
    TEST_ASSERT_EQUAL(55, node.active_root());
}

static void test_root_inlet_last_note_on_wins() {
    NodeConfig c = scale_run(4);
    c.in_bus[2] = ROOT_BUS;
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    rig.bus.note_write(ROOT_BUS, MidiEvent{MIDI_NOTE_ON, 1, 55, 90});
    rig.bus.note_write(ROOT_BUS, MidiEvent{MIDI_NOTE_ON, 1, 57, 90});
    rig.bus.note_write(ROOT_BUS, MidiEvent{MIDI_NOTE_OFF, 1, 55, 0});    // ignored
    expect("-60/0 +59/100 ", rig.edge(node));                             // degree 1 of A
    TEST_ASSERT_EQUAL(57, node.root_note());
}

// A scale change under a sounding note releases the pitch that was sent.
static void test_scale_change_releases_what_was_sent() {
    NoteSequencer node(scale_run(4));
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 +62/100 ", rig.edge(node));
    expect("-62/0 +64/100 ", rig.edge(node));                   // E in major
    node.set_scale_mask(scale_mask(SCALE_NATURAL_MINOR));
    expect("-64/0 +65/100 ", rig.edge(node));                   // released 64, not 63
    expect("-65/0 +60/100 ", rig.edge(node));
    expect("-60/0 +62/100 ", rig.edge(node));
    expect("-62/0 +63/100 ", rig.edge(node));                   // Eb now
}

// A pattern that named no scale plays the module's, so one key change moves
// every sequencer in the patch. The root is the sequencer's own: it is an
// absolute pitch, naming the octave the pattern starts in, and a pitch class
// cannot say that.
static void test_a_sequence_with_no_scale_of_its_own_follows_the_module() {
    NodeConfig c = scale_run(4, 0, 60);                         // no scale named
    NoteSequencer node(c);
    Rig rig;
    global_scale::set(SCALE_MAJOR, 0);
    TEST_ASSERT_EQUAL_HEX16(scale_mask(SCALE_MAJOR), node.active_mask());
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 +62/100 ", rig.edge(node));
    expect("-62/0 +64/100 ", rig.edge(node));                   // E: major

    // Changing the module's key changes the character and releases what was
    // actually sent, exactly as this node's own scale parameter does.
    global_scale::set(SCALE_NATURAL_MINOR, 0);
    expect("-64/0 +65/100 ", rig.edge(node));
    expect("-65/0 +60/100 ", rig.edge(node));
    expect("-60/0 +62/100 ", rig.edge(node));
    expect("-62/0 +63/100 ", rig.edge(node));                   // Eb now

    // And a pattern that names a scale keeps it whatever the module is in.
    node.set_scale_mask(scale_mask(SCALE_MAJOR));
    TEST_ASSERT_EQUAL_HEX16(scale_mask(SCALE_MAJOR), node.active_mask());
    expect("-63/0 +65/100 ", rig.edge(node));
}

// A pitch that leaves 0..127 is skipped, never wrapped, and owes nothing.
static void test_out_of_range_pitch_is_skipped() {
    NodeConfig c = scale_run(2, scale_mask(SCALE_MAJOR), 120);
    set_step(c, 1, 1, 0, 7, 100);                               // 120 + 12 = 132
    NoteSequencer node(c);
    Rig rig;
    expect("+120/100 ", rig.edge(node));
    expect("-120/0 ", rig.edge(node));
    TEST_ASSERT_EQUAL(0, node.sounding_count());
    TEST_ASSERT_EQUAL(NS::NO_PITCH, node.pitch(1, 0));
    expect("+120/100 ", rig.edge(node));
}

// ---------------------------------------------------------------------------
// Per-step velocity, rest, tie, accent, probability
// ---------------------------------------------------------------------------

static void test_velocity_per_step_with_scale_offset_and_accent() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 3);
    c.params[NS::P_VEL_SCALE] = 50;
    c.params[NS::P_VEL_OFFSET] = (uint8_t)(int8_t)-10;
    c.params[NS::P_ACCENT] = 40;
    set_step(c, 1, 0, 0, 0, 100);                               // 100 * 50% - 10 = 40
    set_step(c, 1, 1, 0, 0, 100, 1, NS::FLAG_ACCENT);           // (100 + 40) * 50% - 10 = 60
    set_step(c, 1, 2, 0, 0, 10);                                // 10 * 50% - 10 < 1 -> 1, never a note-off
    NoteSequencer node(c);
    Rig rig;
    expect("+60/40 ", rig.edge(node));
    expect("-60/0 +60/60 ", rig.edge(node));
    expect("-60/0 +60/1 ", rig.edge(node));
}

static void test_rest_plays_nothing_and_releases_on_time() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 3);
    set_step(c, 1, 0, 0, 0, 100);
    set_step(c, 1, 1, 0, 4, 100, 1, NS::FLAG_REST);             // degree kept, step silent
    set_step(c, 1, 2, 0, 2, 100);
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 ", rig.edge(node));                           // the rest: release only
    expect("+64/100 ", rig.edge(node));
    expect("-64/0 +60/100 ", rig.edge(node));
    TEST_ASSERT_EQUAL(4, node.degree(1, 0));
}

// A tie extends the previous note rather than retriggering it, which is not
// the same as a long length: the tied step's own degree is not played.
static void test_tie_extends_rather_than_retriggers() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 4);
    set_step(c, 1, 0, 0, 0, 100);
    set_step(c, 1, 1, 0, 4, 100, 1, NS::FLAG_TIE);
    set_step(c, 1, 2, 0, 4, 100, 1, NS::FLAG_TIE);
    set_step(c, 1, 3, 0, 2, 100);
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("", rig.edge(node));                                 // tied: nothing at all
    expect("", rig.edge(node));
    expect("-60/0 +64/100 ", rig.edge(node));                   // held three steps, then released
    expect("-64/0 +60/100 ", rig.edge(node));
}

// A tie with nothing sounding is a plain step: after a rest it plays.
static void test_tie_after_a_rest_plays() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 3);
    set_step(c, 1, 0, 0, 0, 100);
    set_step(c, 1, 1, 0, 0, 100, 1, NS::FLAG_REST);
    set_step(c, 1, 2, 0, 4, 100, 1, NS::FLAG_TIE);
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 ", rig.edge(node));
    expect("+67/100 ", rig.edge(node));
}

static void test_per_step_probability() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 2);
    set_step(c, 1, 0, 0, 0, 100);                               // always
    set_step(c, 1, 1, 0, 2, 100, 1, 0, 1);                      // 1 percent
    NoteSequencer node(c);
    Rig rig;
    uint16_t certain = 0, unlikely = 0;
    for (uint16_t i = 0; i < 200; i++) {
        for (const MidiEvent& e : rig.edge(node)) {
            if (!is_note_on(e)) continue;
            if (e.data1 == 60) certain++; else unlikely++;
        }
    }
    TEST_ASSERT_EQUAL(100, certain);
    TEST_ASSERT_TRUE(unlikely < 20);
    TEST_ASSERT_EQUAL(1, node.probability(1));
    TEST_ASSERT_EQUAL(100, node.probability(0));
}

// ---------------------------------------------------------------------------
// Note length: exact in edge units, estimated as a sub-step gate
// ---------------------------------------------------------------------------

static void test_length_in_edge_units_releases_after_exactly_n_advances() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 6);
    set_step(c, 1, 0, 0, 0, 100, 3);                            // three edges long
    set_step(c, 1, 1, 0, 0, 100, 1, NS::FLAG_REST);
    set_step(c, 1, 2, 0, 0, 100, 1, NS::FLAG_REST);
    set_step(c, 1, 3, 0, 0, 100, 1, NS::FLAG_REST);
    set_step(c, 1, 4, 0, 2, 100, 4);                            // four, but cut by step 5
    set_step(c, 1, 5, 0, 4, 100);
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));                         // step 0
    expect("", rig.edge(node));                                 // 1
    expect("", rig.edge(node));                                 // 2
    expect("-60/0 ", rig.edge(node));                           // 3: exactly three edges later
    expect("+64/100 ", rig.edge(node));                         // 4
    expect("-64/0 +67/100 ", rig.edge(node));                   // 5: one voice, so the long note is cut
    expect("-67/0 +60/100 ", rig.edge(node));
}

// With a gate percentage, the last edge-unit of a note is shortened to that
// fraction of the measured step period. Before a period has been measured,
// the note is exact.
static void test_sub_step_gate_is_measured_and_labelled_an_estimate() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 3);
    c.params[NS::P_GATE] = 50;
    set_step(c, 1, 0, 0, 0, 100);
    set_step(c, 1, 1, 0, 2, 100, 2);
    set_step(c, 1, 2, 0, 0, 100, 1, NS::FLAG_REST);
    NoteSequencer node(c);
    Rig rig;
    // First edge: no period yet, so the note lasts until the next edge.
    expect("+60/100 ", rig.edge(node));
    TEST_ASSERT_FALSE(node.period_known());
    // Second edge, 40 ms later: the period is now 40 ms. Step 1 is two edges
    // long, so nothing is timed yet.
    expect("-60/0 +64/100 ", rig.edge(node));
    TEST_ASSERT_TRUE(node.period_known());
    TEST_ASSERT_EQUAL_UINT32(2 * HALF_US, node.period_us());
    // Third edge, the rest: 64 enters its last unit, which ends 20 ms in.
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    expect("", rig.collect());
    expect("", rig.pass(node, 10000));                          // +10 ms: nothing yet
    expect("-64/0 ", rig.pass(node, 12000));                    // +22 ms: 50% of 40 ms is up
    rig.now += 18000;
    // Fourth edge: step 0 is one edge long, so it is timed from the start.
    rig.bus.gate_write(ADVANCE, true);
    rig.bus.swap(); node.process(rig.bus, rig.now); rig.bus.swap();
    expect("+60/100 ", rig.collect());
    expect("", rig.pass(node, 10000));
    expect("", rig.pass(node, 5000));
    expect("-60/0 ", rig.pass(node, 6000));
    // The stored lengths are untouched by all this.
    TEST_ASSERT_EQUAL(1, node.step_length(0));
    TEST_ASSERT_EQUAL(2, node.step_length(1));
}

// The clock stopping is the other way to hang a note. After `stall` periods
// without an edge, whatever is sounding is released; 255 never does.
static void test_stall_releases_when_the_advance_stops() {
    NodeConfig c = seq_config(ALGO_NOTE_SEQ, 1);
    set_step(c, 1, 0, 0, 0, 100, 8);                            // eight edges long
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 +60/100 ", rig.edge(node));                   // one voice: retriggered, period 40 ms
    // Four periods of silence = 160 ms from the last edge (20 ms already passed).
    expect("", rig.pass(node, 100000));
    expect("-60/0 ", rig.pass(node, 60000));
    TEST_ASSERT_EQUAL(0, node.sounding_count());

    c.params[NS::P_STALL] = NS::STALL_NEVER;
    NoteSequencer drone(c);
    Rig r2;
    r2.edge(drone); r2.edge(drone);
    expect("", r2.pass(drone, 5000000));
    TEST_ASSERT_EQUAL(1, drone.sounding_count());
}

// ---------------------------------------------------------------------------
// Polyphony
// ---------------------------------------------------------------------------

static void test_poly_step_emits_four_voices_and_releases_all_of_them() {
    NodeConfig c = seq_config(ALGO_POLY_SEQ, 2);
    set_step(c, NS::MAX_VOICES, 0, 0, 0, 100, 1);               // C E G C'
    set_step(c, NS::MAX_VOICES, 0, 1, 2, 90, 1);
    set_step(c, NS::MAX_VOICES, 0, 2, 4, 80, 1);
    set_step(c, NS::MAX_VOICES, 0, 3, 7, 70, 1);
    set_step(c, NS::MAX_VOICES, 1, 0, 1, 100, 1);               // D, voice 1..3 silent
    PolySequencer node(c);
    TEST_ASSERT_EQUAL(NS::MAX_VOICES, node.voices());
    Rig rig;
    expect("+60/100 +64/90 +67/80 +72/70 ", rig.edge(node));
    TEST_ASSERT_EQUAL(4, node.sounding_count());
    expect("-60/0 -64/0 -67/0 -72/0 +62/100 ", rig.edge(node));
    TEST_ASSERT_EQUAL(1, node.sounding_count());
    node.set_root(65);                                          // F, under the D
    expect("-62/0 +65/100 +69/90 +72/80 +77/70 ", rig.edge(node));
}

// The two families share the engine: a gate sequencer and a note sequencer
// at the same length and direction visit steps identically.
static void test_engine_is_shared_with_the_gate_sequencers() {
    for (uint8_t dir = 0; dir < StepEngine::SEQ_RANDOM; dir++) {
        NodeConfig g = node_config(ALGO_STEP_SEQ);
        g.in_bus[0] = ADVANCE; g.out_bus[0] = 5;
        g.params[0] = 5; g.params[1] = dir;
        StepSequencer gate(g);
        NodeConfig n = scale_run(5);
        n.params[NS::P_DIRECTION] = dir;
        NoteSequencer note(n);
        Rig rig;
        for (uint8_t i = 0; i < 20; i++) {
            rig.bus.gate_write(ADVANCE, true);
            rig.bus.swap(); gate.process(rig.bus, rig.now); note.process(rig.bus, rig.now); rig.bus.swap();
            TEST_ASSERT_EQUAL(gate.position(), note.position());
            rig.now += HALF_US;
            rig.bus.swap(); gate.process(rig.bus, rig.now); note.process(rig.bus, rig.now); rig.bus.swap();
            rig.now += HALF_US;
        }
        TEST_ASSERT_EQUAL(gate.steps_taken(), note.steps_taken());
    }
}

// Reset sends the sequence back to its first step, and the note that was
// sounding is released on that edge like any other.
static void test_reset_inlet() {
    NodeConfig c = scale_run(4);
    c.in_bus[1] = RESET;
    NoteSequencer node(c);
    Rig rig;
    expect("+60/100 ", rig.edge(node));
    expect("-60/0 +62/100 ", rig.edge(node));
    rig.reset_pulse(node);
    expect("-62/0 +60/100 ", rig.edge(node));
    TEST_ASSERT_EQUAL(0, node.position());
}

// ---------------------------------------------------------------------------
// No hanging notes: everything that can change under a sounding note does,
// and the ledger balances at the end.
// ---------------------------------------------------------------------------

struct NoteBalance {
    int8_t sounding[16][128];
    bool went_negative;
    NoteBalance() : sounding(), went_negative(false) {}
    void observe(const std::vector<MidiEvent>& events) {
        for (const MidiEvent& e : events) {
            int8_t& s = sounding[(e.channel - 1) & 15][e.data1];
            if (is_note_on(e)) s++;
            else if (is_note_off(e)) { s--; if (s < 0) went_negative = true; }
        }
    }
    uint16_t total() const {
        uint16_t n = 0;
        for (uint8_t c = 0; c < 16; c++) for (uint8_t i = 0; i < 128; i++) if (sounding[c][i] > 0) n = (uint16_t)(n + sounding[c][i]);
        return n;
    }
};

template <class T>
static void assert_no_hanging_notes(uint8_t voices, uint32_t seed) {
    NodeConfig c = seq_config(voices == 1 ? ALGO_NOTE_SEQ : ALGO_POLY_SEQ, 16);
    c.in_bus[1] = RESET;
    c.in_bus[2] = ROOT_BUS;
    c.params[NS::P_GATE] = 60;
    Xorshift32 script(seed);
    for (uint8_t s = 0; s < MAX_SEQUENCE_LEN; s++) {
        for (uint8_t v = 0; v < voices; v++) set_step(c, voices, s, v, (int8_t)(script.below(24) - 8), script.chance(80) ? (uint8_t)(1 + script.below(127)) : 0);
        const uint8_t flags = (uint8_t)((script.chance(15) ? NS::FLAG_REST : 0) | (script.chance(25) ? NS::FLAG_TIE : 0) | (script.chance(20) ? NS::FLAG_ACCENT : 0));
        uint8_t* b = &c.params[NS::STEP_BASE + s * NS::stride(voices)];
        b[voices * 2] = (uint8_t)((1 + script.below(4)) | flags);
        b[voices * 2 + 1] = (uint8_t)script.below(101);
    }
    T node(c);
    Rig rig;
    NoteBalance balance;
    const uint16_t scales[4] = {scale_mask(SCALE_MAJOR), scale_mask(SCALE_PENTATONIC_MINOR), scale_mask(SCALE_BLUES), 0};
    for (uint16_t i = 0; i < 2000; i++) {
        // Something changes under the sounding notes on most edges.
        switch (script.below(8)) {
            case 0: node.set_root((uint8_t)(36 + script.below(60))); break;
            case 1: node.set_scale_mask(scales[script.below(4)]); break;
            case 2: node.set_length((uint8_t)(1 + script.below(MAX_SEQUENCE_LEN))); break;
            case 3: node.set_step(script.below(MAX_SEQUENCE_LEN), script.below(voices), (int8_t)(script.below(30) - 10), script.below(128)); break;
            case 4: rig.bus.note_write(ROOT_BUS, MidiEvent{MIDI_NOTE_ON, 1, (uint8_t)(40 + script.below(40)), 100}); break;
            case 5: rig.bus.gate_write(RESET, true); break;
            default: break;
        }
        // Irregular timing, so the estimated gate is sometimes wrong.
        rig.now += script.below(30) * 1000u;
        balance.observe(rig.edge(node));
        TEST_ASSERT_FALSE(balance.went_negative);
        TEST_ASSERT_EQUAL_MESSAGE(0, rig.bus.note_overflows(OUT), "the node overflowed the output bus");
    }
    // The clock stops: the stall timeout releases the rest.
    for (uint8_t i = 0; i < 10; i++) balance.observe(rig.pass(node, 100000));
    TEST_ASSERT_FALSE(balance.went_negative);
    TEST_ASSERT_EQUAL(0, balance.total());
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

static void test_mono_hangs_nothing_under_every_change() {
    assert_no_hanging_notes<NoteSequencer>(1, 11);
    assert_no_hanging_notes<NoteSequencer>(1, 12);
}

static void test_poly_hangs_nothing_under_every_change() {
    assert_no_hanging_notes<PolySequencer>(NS::MAX_VOICES, 21);
    assert_no_hanging_notes<PolySequencer>(NS::MAX_VOICES, 22);
}

// ---------------------------------------------------------------------------
// Through the master: a patch swap mid-note releases the note
// ---------------------------------------------------------------------------

static Patch sequencer_patch() {
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);                    // tick -> gate 0, /6
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 6;
    p.nodes[1] = scale_run(4);                                   // gate 0 -> note 3
    p.nodes[1].params[NS::P_STALL] = NS::STALL_NEVER;
    set_step(p.nodes[1], 1, 0, 0, 0, 100, 4);                    // a long note
    p.n_nodes = 2;
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 3};
    return p;
}

static void test_patch_swap_mid_note_releases_it() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(sequencer_patch()));
    master.setup();
    uint32_t now = 0;
    for (uint32_t t = 0; t < 8 * CLOCK_SUBTICK; t++) { master.clock().advance(); master.pass(now); now += 300; }
    for (int i = 0; i < 4; i++) { master.pass(now); now += 300; }
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, midi.messages[0].type);
    TEST_ASSERT_EQUAL(60, midi.messages[0].d1);

    // Swap to an unrelated patch: the note-off goes out through the old
    // patch's MIDI port before anything is destroyed.
    Patch other = empty_patch();
    other.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(other));
    master.setup();
    TEST_ASSERT_EQUAL(2, midi.messages.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, midi.messages[1].type);
    TEST_ASSERT_EQUAL(60, midi.messages[1].d1);
    TEST_ASSERT_EQUAL(mmMIDI_USB_0, midi.messages[1].target);
    // And unload() alone does the same.
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(sequencer_patch()));
    master.setup();
    midi.clear();
    for (uint32_t t = 0; t < 8 * CLOCK_SUBTICK; t++) { master.clock().advance(); master.pass(now); now += 300; }
    for (int i = 0; i < 4; i++) { master.pass(now); now += 300; }
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    master.unload();
    TEST_ASSERT_EQUAL(2, midi.messages.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, midi.messages[1].type);
    TEST_ASSERT_EQUAL(0, master.node_count());
}

static void test_descriptors_and_sizes() {
    TEST_ASSERT_EQUAL(NS::param_count(1), NoteSequencer::descriptor.n_params);
    TEST_ASSERT_EQUAL(NS::param_count(NS::MAX_VOICES), PolySequencer::descriptor.n_params);
    TEST_ASSERT_TRUE(PolySequencer::descriptor.n_params <= N_PARAM);
    TEST_ASSERT_TRUE(sizeof(NoteSequencer) <= NODE_SLOT_SIZE);
    TEST_ASSERT_TRUE(sizeof(PolySequencer) <= NODE_SLOT_SIZE);
    TEST_ASSERT_EQUAL(Domain::Note, NoteSequencer::descriptor.in_domain[2]);
    TEST_ASSERT_EQUAL(1, NoteSequencer::descriptor.min_in);
    NodeConfig c = scale_run(4);
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
    c.algorithm_id = ALGO_POLY_SEQ;
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
    c.in_bus[2] = N_NOTE_BUS;
    TEST_ASSERT_EQUAL(CONFIG_INLET_OUT_OF_RANGE, registry::validate(c));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_degree_wrapping_in_7_and_5_note_scales);
    RUN_TEST(test_mono_sequence_emits_the_expected_pitches_twice_over);
    RUN_TEST(test_root_change_releases_what_was_sent_and_transposes_the_rest);
    RUN_TEST(test_root_inlet_last_note_on_wins);
    RUN_TEST(test_a_pattern_keeps_its_anchor_until_the_key_names_a_register);
    RUN_TEST(test_the_root_inlet_outranks_the_key_register);
    RUN_TEST(test_scale_change_releases_what_was_sent);
    RUN_TEST(test_a_sequence_with_no_scale_of_its_own_follows_the_module);
    RUN_TEST(test_out_of_range_pitch_is_skipped);
    RUN_TEST(test_velocity_per_step_with_scale_offset_and_accent);
    RUN_TEST(test_rest_plays_nothing_and_releases_on_time);
    RUN_TEST(test_tie_extends_rather_than_retriggers);
    RUN_TEST(test_tie_after_a_rest_plays);
    RUN_TEST(test_per_step_probability);
    RUN_TEST(test_length_in_edge_units_releases_after_exactly_n_advances);
    RUN_TEST(test_sub_step_gate_is_measured_and_labelled_an_estimate);
    RUN_TEST(test_stall_releases_when_the_advance_stops);
    RUN_TEST(test_poly_step_emits_four_voices_and_releases_all_of_them);
    RUN_TEST(test_engine_is_shared_with_the_gate_sequencers);
    RUN_TEST(test_reset_inlet);
    RUN_TEST(test_mono_hangs_nothing_under_every_change);
    RUN_TEST(test_poly_hangs_nothing_under_every_change);
    RUN_TEST(test_patch_swap_mid_note_releases_it);
    RUN_TEST(test_descriptors_and_sizes);
    return UNITY_END();
}
