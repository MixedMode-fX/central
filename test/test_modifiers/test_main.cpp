#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/held_notes.h"
#include "midi/sounding_notes.h"
#include "midi/scale.h"
#include "midi/note_event.h"
#include "algorithm/midi/transpose.h"
#include "algorithm/midi/note_priority.h"
#include "algorithm/midi/velocity_curve.h"
#include "algorithm/midi/chord.h"
#include "algorithm/midi/quantise.h"
#include "algorithm/midi/probability.h"
#include "algorithm/midi/arpeggiator.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Test rig: one pass around a node, exactly as MixedModeMaster runs it.
// Input events are written, swapped in, the node runs, and its output is
// swapped out and collected.
// ---------------------------------------------------------------------------

static std::vector<MidiEvent> run_pass(BusManager& bus, Node& node,
                                       uint8_t out_bus, uint32_t now_us = 0) {
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    std::vector<MidiEvent> out;
    const uint8_t n = bus.note_count(out_bus);
    for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(out_bus, i));
    return out;
}

static MidiEvent on(uint8_t note, uint8_t velocity = 100, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_ON, channel, note, velocity};
}
static MidiEvent off(uint8_t note, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_OFF, channel, note, 0};
}

// Every note-on the modifier emitted must be released by the time its source
// is. This ledger is what the "no hanging notes" tests assert against.
struct NoteBalance {
    int8_t sounding[128];
    bool went_negative;

    NoteBalance() : sounding(), went_negative(false) {}

    void observe(const std::vector<MidiEvent>& events) {
        for (const MidiEvent& e : events) {
            if (is_note_on(e)) sounding[e.data1]++;
            else if (is_note_off(e)) {
                sounding[e.data1]--;
                if (sounding[e.data1] < 0) went_negative = true;
            }
        }
    }
    uint16_t total() const {
        uint16_t n = 0;
        for (uint8_t i = 0; i < 128; i++) n = (uint16_t)(n + (sounding[i] > 0 ? sounding[i] : 0));
        return n;
    }
};

// A deterministic stream of note-ons and note-offs, so a failure can be
// reproduced exactly.
struct NoteScript {
    uint32_t state;
    explicit NoteScript(uint32_t seed) : state(seed) {}
    uint32_t next() { state = state * 1664525u + 1013904223u; return state; }
    uint8_t note() { return (uint8_t)(36 + (next() >> 16) % 48); }
    bool on_event() { return ((next() >> 16) & 3) != 0; }
};

// ---------------------------------------------------------------------------
// HeldNotes: the four bugs the first attempt shipped
// ---------------------------------------------------------------------------

static void test_held_notes_arrival_order_survives_out_of_order_offs() {
    HeldNotes held;
    HeldNote evicted = {0, 0, 0};
    bool did = false;
    held.add(60, 100, 1, evicted, did);
    held.add(64, 101, 1, evicted, did);
    held.add(67, 102, 1, evicted, did);
    held.remove(64);                       // the middle one goes first
    TEST_ASSERT_EQUAL(2, held.count());
    TEST_ASSERT_EQUAL(60, held.at(0).note);
    TEST_ASSERT_EQUAL(67, held.at(1).note);
    TEST_ASSERT_EQUAL(67, held.latest());
    held.add(62, 103, 1, evicted, did);    // a new note is the latest
    TEST_ASSERT_EQUAL(62, held.latest());
    TEST_ASSERT_EQUAL(60, held.sorted(0).note);   // pitch order is separate
    TEST_ASSERT_EQUAL(62, held.sorted(1).note);
    TEST_ASSERT_EQUAL(67, held.sorted(2).note);
}

// "Empty" has exactly one representation, not 128 in one query and 255 in
// another.
static void test_held_notes_empty_has_one_representation() {
    HeldNotes held;
    TEST_ASSERT_EQUAL(HeldNotes::NONE, held.lowest());
    TEST_ASSERT_EQUAL(HeldNotes::NONE, held.highest());
    TEST_ASSERT_EQUAL(HeldNotes::NONE, held.latest());
    TEST_ASSERT_EQUAL(HeldNotes::NONE, held.at(0).note);
    TEST_ASSERT_EQUAL(HeldNotes::NONE, held.sorted(0).note);
    TEST_ASSERT_EQUAL(0xFF, HeldNotes::NONE);
}

// Overflow reports the note it evicted instead of overwriting it silently.
static void test_held_notes_overflow_reports_the_evicted_note() {
    HeldNotes held;
    HeldNote evicted = {0, 0, 0};
    bool did = false;
    for (uint8_t i = 0; i < HeldNotes::CAPACITY; i++) {
        held.add((uint8_t)(40 + i), 100, 1, evicted, did);
        TEST_ASSERT_FALSE(did);
    }
    held.add(90, 100, 1, evicted, did);
    TEST_ASSERT_TRUE(did);
    TEST_ASSERT_EQUAL(40, evicted.note);           // the oldest went
    TEST_ASSERT_EQUAL(HeldNotes::CAPACITY, held.count());
    TEST_ASSERT_FALSE(held.contains(40));
    TEST_ASSERT_TRUE(held.contains(90));
}

// A repeated note keeps its place in the arrival order rather than jumping to
// the end and losing "latest" its meaning.
static void test_held_notes_repeat_updates_in_place() {
    HeldNotes held;
    HeldNote evicted = {0, 0, 0};
    bool did = false;
    held.add(60, 100, 1, evicted, did);
    held.add(64, 100, 1, evicted, did);
    held.add(60, 40, 1, evicted, did);
    TEST_ASSERT_EQUAL(2, held.count());
    TEST_ASSERT_EQUAL(60, held.at(0).note);
    TEST_ASSERT_EQUAL(40, held.at(0).velocity);
    TEST_ASSERT_EQUAL(64, held.latest());
}

// The panic path emits note-offs and nothing else. The original called the
// note-off *path*, which re-triggered the next priority note - so the panic
// routine sent note-ons.
static void test_release_all_emits_only_note_offs() {
    BusManager bus;
    SoundingNotes sounding;
    sounding.emit(bus, 0, 60, 60, 100, 1);
    sounding.emit(bus, 0, 64, 64, 100, 1);
    bus.swap();
    bus.swap();                                    // clear what emit() wrote

    TEST_ASSERT_EQUAL(2, sounding.release_all(bus, 0));
    bus.swap();
    TEST_ASSERT_EQUAL(2, bus.note_count(0));
    for (uint8_t i = 0; i < 2; i++) TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, bus.note_read(0, i).type);
    TEST_ASSERT_EQUAL(0, sounding.count());
}

// A note that cannot be recorded is not sent: what cannot be released is
// never emitted.
static void test_sounding_notes_refuses_what_it_cannot_release() {
    BusManager bus;
    SoundingNotes sounding;
    for (uint8_t i = 0; i < SoundingNotes::CAPACITY; i++) {
        TEST_ASSERT_TRUE(sounding.emit(bus, 0, i, i, 100, 1));
    }
    TEST_ASSERT_FALSE(sounding.emit(bus, 0, 99, 99, 100, 1));
    TEST_ASSERT_EQUAL_UINT32(1, sounding.refused());
    TEST_ASSERT_EQUAL(SoundingNotes::CAPACITY, sounding.count());
}

// ---------------------------------------------------------------------------
// Scales
// ---------------------------------------------------------------------------

static void test_scale_masks_and_quantisation() {
    TEST_ASSERT_EQUAL_HEX16(0x0FFF, scale_mask(SCALE_CHROMATIC));
    const uint16_t major = scale_mask(SCALE_MAJOR);
    const uint8_t degrees[7] = {0, 2, 4, 5, 7, 9, 11};
    for (uint8_t i = 0; i < 7; i++) TEST_ASSERT_TRUE(major & (uint16_t)(1u << degrees[i]));
    TEST_ASSERT_FALSE(major & (1u << 1));
    TEST_ASSERT_FALSE(major & (1u << 6));

    // C major: C stays, C# goes up to D, F# goes up to G.
    TEST_ASSERT_EQUAL(60, scale_quantise(60, 0, major));
    TEST_ASSERT_EQUAL(62, scale_quantise(61, 0, major));
    TEST_ASSERT_EQUAL(67, scale_quantise(66, 0, major));
    // Chromatic passes everything through untouched.
    for (uint8_t n = 0; n < 128; n++) {
        TEST_ASSERT_EQUAL(n, scale_quantise(n, 0, scale_mask(SCALE_CHROMATIC)));
    }
    // Nothing ever leaves the MIDI range, whatever the root.
    for (uint8_t root = 0; root < 12; root++) {
        TEST_ASSERT_EQUAL(scale_quantise(127, root, scale_mask(SCALE_PENTATONIC_MINOR)),
                          scale_quantise(127, root, scale_mask(SCALE_PENTATONIC_MINOR)));
        TEST_ASSERT_TRUE(scale_quantise(0, root, scale_mask(SCALE_BLUES)) <= 127);
    }
}

// ---------------------------------------------------------------------------
// Transpose: the proving ground for the note-off contract
// ---------------------------------------------------------------------------

static void test_transpose_releases_what_it_sent_after_the_offset_moves() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_TRANSPOSE);
    c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = 12;
    Transpose node(c);

    bus.note_write(0, on(60));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(72, out[0].data1);

    node.set_semitones(-5);                       // the offset moves under the note

    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[0].type);
    TEST_ASSERT_EQUAL(72, out[0].data1);          // as sent, not 55
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// A note dropped for leaving the range takes its note-off with it.
static void test_transpose_drops_out_of_range_notes_and_their_offs() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_TRANSPOSE);
    c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = 24;
    Transpose node(c);

    bus.note_write(0, on(120));                   // 144: gone
    bus.note_write(0, on(60));                    // 84: fine
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(84, out[0].data1);

    bus.note_write(0, off(120));
    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());             // no off for the dropped note
    TEST_ASSERT_EQUAL(84, out[0].data1);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// ---------------------------------------------------------------------------
// Note priority
// ---------------------------------------------------------------------------

static void test_note_priority_modes() {
    const uint8_t modes[3] = {NotePriority::PRIORITY_LOW, NotePriority::PRIORITY_HIGH,
                              NotePriority::PRIORITY_LATEST};
    const uint8_t expected[3] = {60, 67, 64};     // played 60, then 67 and 64
    for (uint8_t m = 0; m < 3; m++) {
        BusManager bus;
        NodeConfig c = node_config(ALGO_NOTE_PRIORITY);
        c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = modes[m];
        NotePriority node(c);

        // The voice, followed across passes: one note at a time, whatever the
        // mode, and only one note-on outstanding at any moment.
        uint8_t voice = HeldNotes::NONE;
        uint8_t sounding = 0;

        bus.note_write(0, on(60));
        for (const MidiEvent& e : run_pass(bus, node, 1)) {
            if (is_note_on(e)) { voice = e.data1; sounding++; } else sounding--;
        }
        TEST_ASSERT_EQUAL(60, voice);
        TEST_ASSERT_EQUAL(1, sounding);

        bus.note_write(0, on(67));
        bus.note_write(0, on(64));
        for (const MidiEvent& e : run_pass(bus, node, 1)) {
            if (is_note_on(e)) { voice = e.data1; sounding++; } else sounding--;
        }
        TEST_ASSERT_EQUAL(expected[m], voice);
        TEST_ASSERT_EQUAL(1, sounding);           // monophonic, always
    }
}

// Releasing the sounding note hands the voice to the next held note - which
// is only possible because arrival order is kept.
static void test_note_priority_falls_back_on_release() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_NOTE_PRIORITY);
    c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = NotePriority::PRIORITY_HIGH;
    NotePriority node(c);

    bus.note_write(0, on(60));
    bus.note_write(0, on(72));
    run_pass(bus, node, 1);

    bus.note_write(0, off(72));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[0].type);
    TEST_ASSERT_EQUAL(72, out[0].data1);
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, out[1].type);
    TEST_ASSERT_EQUAL(60, out[1].data1);

    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[0].type);
}

// ---------------------------------------------------------------------------
// Velocity
// ---------------------------------------------------------------------------

static void test_velocity_curves_never_reach_zero() {
    for (uint8_t curve = 0; curve <= VelocityCurve::CURVE_HARD; curve++) {
        NodeConfig c = node_config(ALGO_VELOCITY);
        c.in_bus[0] = 0; c.out_bus[0] = 1;
        c.params[0] = curve;
        c.params[1] = 1;                          // 1 percent: the crushing case
        VelocityCurve node(c);
        for (uint8_t v = 1; v < 128; v++) {
            const uint8_t result = node.apply(v);
            TEST_ASSERT_TRUE(result >= 1);        // 0 would be a note-off
            TEST_ASSERT_TRUE(result <= 127);
        }
    }
}

static void test_velocity_curve_shapes_and_passes_offs() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_VELOCITY);
    c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = VelocityCurve::CURVE_SOFT;
    VelocityCurve node(c);
    TEST_ASSERT_EQUAL(127, node.apply(127));
    TEST_ASSERT_TRUE(node.apply(64) > 64);        // soft lifts the middle
    TEST_ASSERT_EQUAL(1, node.apply(1) >= 1 ? 1 : 0);

    bus.note_write(0, on(60, 64));
    bus.note_write(0, off(60));
    bus.note_write(0, MidiEvent{MIDI_CONTROL_CHANGE, 1, 64, 127});
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_TRUE(out[0].data2 > 64);
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[1].type);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, out[2].type);
}

// ---------------------------------------------------------------------------
// Chord
// ---------------------------------------------------------------------------

static void test_chord_emits_the_interval_set_and_releases_all_of_it() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_CHORD);
    c.in_bus[0] = 0; c.out_bus[0] = 1;
    c.params[0] = 2;
    c.params[1] = 4;                              // major third
    c.params[2] = 7;                              // fifth
    Chord node(c);

    bus.note_write(0, on(60));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(64, out[1].data1);
    TEST_ASSERT_EQUAL(67, out[2].data1);

    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    for (const MidiEvent& e : out) TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, e.type);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// ---------------------------------------------------------------------------
// Quantise
// ---------------------------------------------------------------------------

static void test_quantise_snaps_and_releases_what_it_sent() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_QUANTISE);
    c.in_bus[0] = 0; c.in_bus[1] = NO_BUS; c.out_bus[0] = 1;
    c.params[0] = SCALE_MAJOR;
    c.params[1] = 0;                              // C
    Quantise node(c);

    bus.note_write(0, on(61));                    // C# -> D
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(62, out[0].data1);

    node.set_root(2);                             // the root moves to D
    node.set_scale(SCALE_PENTATONIC_MINOR);

    bus.note_write(0, off(61));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[0].type);
    TEST_ASSERT_EQUAL(62, out[0].data1);          // the pitch that was sent
}

static void test_quantise_takes_its_root_from_a_bus() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_QUANTISE);
    c.in_bus[0] = 0; c.in_bus[1] = 2; c.out_bus[0] = 1;
    c.params[0] = SCALE_MAJOR;
    Quantise node(c);

    bus.note_write(2, on(62));                    // root D, from the root inlet
    bus.note_write(0, on(60));                    // C is not in D major -> C#
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, node.root_note());
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(61, out[0].data1);
}

// ---------------------------------------------------------------------------
// Probability
// ---------------------------------------------------------------------------

static void test_probability_pairs_every_note_it_passes() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_PROBABILITY);
    c.in_bus[0] = 0; c.out_bus[0] = 1; c.params[0] = 50;
    Probability node(c);

    NoteBalance balance;
    uint16_t passed = 0;
    for (uint16_t i = 0; i < 200; i++) {
        const uint8_t note = (uint8_t)(48 + (i % 12));
        bus.note_write(0, on(note));
        std::vector<MidiEvent> out = run_pass(bus, node, 1);
        passed = (uint16_t)(passed + out.size());
        balance.observe(out);
        bus.note_write(0, off(note));
        out = run_pass(bus, node, 1);
        balance.observe(out);
    }
    TEST_ASSERT_FALSE(balance.went_negative);
    TEST_ASSERT_EQUAL(0, balance.total());        // nothing left sounding
    TEST_ASSERT_TRUE(passed > 40 && passed < 160);  // it did drop some, not all
}

static void test_probability_defaults_to_passing_everything() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_PROBABILITY);
    c.in_bus[0] = 0; c.out_bus[0] = 1;            // params zeroed
    Probability node(c);
    for (uint8_t i = 0; i < 20; i++) {
        bus.note_write(0, on((uint8_t)(60 + i)));
        TEST_ASSERT_EQUAL(1, run_pass(bus, node, 1).size());
    }
}

// ---------------------------------------------------------------------------
// Arpeggiator
// ---------------------------------------------------------------------------

static void hold_triad(BusManager& bus, uint8_t note_bus) {
    bus.note_write(note_bus, on(60, 100));
    bus.note_write(note_bus, on(67, 101));
    bus.note_write(note_bus, on(64, 102));
}

// One note per advance edge, in the configured order - the acceptance test.
static void test_arpeggiator_one_note_per_edge_in_order() {
    const uint8_t modes[3] = {Arpeggiator::ARP_UP, Arpeggiator::ARP_DOWN,
                              Arpeggiator::ARP_AS_PLAYED};
    const uint8_t expected[3][6] = {
        {60, 64, 67, 60, 64, 67},      // up: pitch order
        {67, 64, 60, 67, 64, 60},      // down
        {60, 67, 64, 60, 67, 64},      // as played
    };
    for (uint8_t m = 0; m < 3; m++) {
        BusManager bus;
        NodeConfig c = node_config(ALGO_ARPEGGIATOR);
        c.in_bus[0] = 0; c.in_bus[1] = 0; c.in_bus[2] = NO_BUS; c.out_bus[0] = 1;
        c.params[0] = modes[m];
        Arpeggiator node(c);

        hold_triad(bus, 0);
        run_pass(bus, node, 1);
        TEST_ASSERT_EQUAL(3, node.held_count());

        for (uint8_t step = 0; step < 6; step++) {
            bus.gate_write(0, true);                       // rising edge
            std::vector<MidiEvent> out = run_pass(bus, node, 1);
            uint8_t ons = 0;
            uint8_t played = HeldNotes::NONE;
            for (const MidiEvent& e : out) if (is_note_on(e)) { ons++; played = e.data1; }
            TEST_ASSERT_EQUAL(1, ons);                     // exactly one per edge
            TEST_ASSERT_EQUAL(expected[m][step], played);
            bus.gate_write(0, false);                      // falling edge: nothing
            out = run_pass(bus, node, 1);
            for (const MidiEvent& e : out) TEST_ASSERT_FALSE(is_note_on(e));
        }
    }
}

static void test_arpeggiator_up_down_does_not_repeat_the_endpoints() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.in_bus[2] = NO_BUS; c.out_bus[0] = 1;
    c.params[0] = Arpeggiator::ARP_UP_DOWN;
    Arpeggiator node(c);

    hold_triad(bus, 0);
    run_pass(bus, node, 1);

    const uint8_t expected[8] = {60, 64, 67, 64, 60, 64, 67, 64};
    for (uint8_t step = 0; step < 8; step++) {
        bus.gate_write(0, true);
        std::vector<MidiEvent> out = run_pass(bus, node, 1);
        uint8_t played = HeldNotes::NONE;
        for (const MidiEvent& e : out) if (is_note_on(e)) played = e.data1;
        TEST_ASSERT_EQUAL(expected[step], played);
        bus.gate_write(0, false);
        run_pass(bus, node, 1);
    }
}

static void test_arpeggiator_octave_range_and_reset() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.in_bus[2] = 1; c.out_bus[0] = 1;
    c.params[1] = 2;                                   // two octaves
    Arpeggiator node(c);

    hold_triad(bus, 0);
    run_pass(bus, node, 1);

    const uint8_t expected[6] = {60, 64, 67, 72, 76, 79};
    for (uint8_t step = 0; step < 4; step++) {
        bus.gate_write(0, true);
        std::vector<MidiEvent> out = run_pass(bus, node, 1);
        uint8_t played = HeldNotes::NONE;
        for (const MidiEvent& e : out) if (is_note_on(e)) played = e.data1;
        TEST_ASSERT_EQUAL(expected[step], played);
        bus.gate_write(0, false);
        run_pass(bus, node, 1);
    }
    // A pulse on the reset inlet sends the figure back to its first step.
    bus.gate_write(1, true);
    run_pass(bus, node, 1);
    bus.gate_write(1, false);
    bus.gate_write(0, true);
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    uint8_t played = HeldNotes::NONE;
    for (const MidiEvent& e : out) if (is_note_on(e)) played = e.data1;
    TEST_ASSERT_EQUAL(60, played);
}

// Lifting the chord releases whatever was sounding: no note survives the
// hand leaving the keyboard.
static void test_arpeggiator_releases_when_the_chord_is_lifted() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.in_bus[2] = NO_BUS; c.out_bus[0] = 1;
    Arpeggiator node(c);

    NoteBalance balance;
    hold_triad(bus, 0);
    balance.observe(run_pass(bus, node, 1));
    bus.gate_write(0, true);
    balance.observe(run_pass(bus, node, 1));
    TEST_ASSERT_EQUAL(1, balance.total());

    bus.note_write(0, off(60));
    bus.note_write(0, off(64));
    bus.note_write(0, off(67));
    balance.observe(run_pass(bus, node, 1));
    TEST_ASSERT_EQUAL(0, balance.total());
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// A fixed gate length releases on time rather than on the next step.
static void test_arpeggiator_gate_length_releases_early() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.in_bus[2] = NO_BUS; c.out_bus[0] = 1;
    c.params[2] = 10;                                  // 10 ms
    Arpeggiator node(c);

    hold_triad(bus, 0);
    run_pass(bus, node, 0);
    bus.gate_write(0, true);
    NoteBalance balance;
    balance.observe(run_pass(bus, node, 1, 1000));
    TEST_ASSERT_EQUAL(1, balance.total());
    bus.gate_write(0, false);
    balance.observe(run_pass(bus, node, 1, 5000));
    TEST_ASSERT_EQUAL(1, balance.total());             // still inside the gate
    balance.observe(run_pass(bus, node, 1, 20000));
    TEST_ASSERT_EQUAL(0, balance.total());             // released without a new step
}

// ---------------------------------------------------------------------------
// The rule that governs every modifier: no hanging notes, ever
// ---------------------------------------------------------------------------

template <class T>
static void assert_no_hanging_notes(NodeConfig config, uint8_t in_bus, uint8_t out_bus,
                                    uint32_t seed, bool with_gate) {
    BusManager bus;
    T node(config);
    NoteBalance balance;
    NoteScript script(seed);
    uint8_t held[16] = {0};
    uint8_t n_held = 0;
    uint32_t now = 0;

    // The source plays like a keyboard: it never sends a note-on for a note it
    // is already holding, and never a note-off for one it is not. Anything
    // left sounding at the end is the modifier's doing, not the script's.
    for (uint16_t i = 0; i < 600; i++) {
        bool play = script.on_event() || n_held == 0;
        if (n_held >= 16) play = false;
        if (play) {
            const uint8_t note = script.note();
            bool already = false;
            for (uint8_t j = 0; j < n_held; j++) if (held[j] == note) already = true;
            if (!already) {
                bus.note_write(in_bus, on(note, (uint8_t)(1 + (note % 126))));
                held[n_held++] = note;
            }
        } else {
            const uint8_t index = (uint8_t)(script.next() % n_held);
            bus.note_write(in_bus, off(held[index]));
            for (uint8_t j = index; j + 1u < n_held; j++) held[j] = held[j + 1];
            n_held--;
        }
        if (with_gate) bus.gate_write(1, (i & 1) != 0);
        balance.observe(run_pass(bus, node, out_bus, now));
        now += 5000;
        // A modifier must never release a note it did not emit.
        TEST_ASSERT_FALSE(balance.went_negative);
    }

    // Everything the source is still holding is released, a few per pass: a
    // note bus carries NOTE_QUEUE_DEPTH events per pass, and a test that
    // overflowed the bus would be proving nothing about the node.
    while (n_held > 0) {
        for (uint8_t k = 0; k < 4 && n_held > 0; k++) bus.note_write(in_bus, off(held[--n_held]));
        now += 5000;
        balance.observe(run_pass(bus, node, out_bus, now));
    }
    // And a last pass with the gate low, for anything waiting on an edge.
    if (with_gate) bus.gate_write(1, false);
    balance.observe(run_pass(bus, node, out_bus, now + 100000));

    TEST_ASSERT_FALSE(balance.went_negative);
    TEST_ASSERT_EQUAL_MESSAGE(0, bus.note_overflows(in_bus), "the test overflowed the input bus");
    TEST_ASSERT_EQUAL_MESSAGE(0, bus.note_overflows(out_bus), "the node overflowed the output bus");
    TEST_ASSERT_EQUAL(0, balance.total());
}

static NodeConfig modifier_config(uint8_t id) {
    NodeConfig c = node_config(id);
    c.in_bus[0] = 0;
    c.out_bus[0] = 3;
    return c;
}

static void test_transpose_hangs_nothing() {
    NodeConfig c = modifier_config(ALGO_TRANSPOSE);
    c.params[0] = (uint8_t)(int8_t)-19;
    assert_no_hanging_notes<Transpose>(c, 0, 3, 1, false);
}

static void test_note_priority_hangs_nothing() {
    assert_no_hanging_notes<NotePriority>(modifier_config(ALGO_NOTE_PRIORITY), 0, 3, 2, false);
}

static void test_velocity_curve_hangs_nothing() {
    assert_no_hanging_notes<VelocityCurve>(modifier_config(ALGO_VELOCITY), 0, 3, 3, false);
}

static void test_chord_hangs_nothing() {
    NodeConfig c = modifier_config(ALGO_CHORD);
    c.params[0] = 3; c.params[1] = 3; c.params[2] = 7; c.params[3] = 12;
    assert_no_hanging_notes<Chord>(c, 0, 3, 4, false);
}

static void test_quantise_hangs_nothing() {
    NodeConfig c = modifier_config(ALGO_QUANTISE);
    c.params[0] = SCALE_BLUES;
    assert_no_hanging_notes<Quantise>(c, 0, 3, 5, false);
}

static void test_probability_hangs_nothing() {
    NodeConfig c = modifier_config(ALGO_PROBABILITY);
    c.params[0] = 60;
    assert_no_hanging_notes<Probability>(c, 0, 3, 6, false);
}

static void test_arpeggiator_hangs_nothing_in_any_mode() {
    for (uint8_t mode = 0; mode <= Arpeggiator::ARP_AS_PLAYED; mode++) {
        NodeConfig c = modifier_config(ALGO_ARPEGGIATOR);
        c.in_bus[1] = 1;
        c.params[0] = mode;
        c.params[1] = 3;
        assert_no_hanging_notes<Arpeggiator>(c, 0, 3, 7u + mode, true);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_held_notes_arrival_order_survives_out_of_order_offs);
    RUN_TEST(test_held_notes_empty_has_one_representation);
    RUN_TEST(test_held_notes_overflow_reports_the_evicted_note);
    RUN_TEST(test_held_notes_repeat_updates_in_place);
    RUN_TEST(test_release_all_emits_only_note_offs);
    RUN_TEST(test_sounding_notes_refuses_what_it_cannot_release);
    RUN_TEST(test_scale_masks_and_quantisation);
    RUN_TEST(test_transpose_releases_what_it_sent_after_the_offset_moves);
    RUN_TEST(test_transpose_drops_out_of_range_notes_and_their_offs);
    RUN_TEST(test_note_priority_modes);
    RUN_TEST(test_note_priority_falls_back_on_release);
    RUN_TEST(test_velocity_curves_never_reach_zero);
    RUN_TEST(test_velocity_curve_shapes_and_passes_offs);
    RUN_TEST(test_chord_emits_the_interval_set_and_releases_all_of_it);
    RUN_TEST(test_quantise_snaps_and_releases_what_it_sent);
    RUN_TEST(test_quantise_takes_its_root_from_a_bus);
    RUN_TEST(test_probability_pairs_every_note_it_passes);
    RUN_TEST(test_probability_defaults_to_passing_everything);
    RUN_TEST(test_arpeggiator_one_note_per_edge_in_order);
    RUN_TEST(test_arpeggiator_up_down_does_not_repeat_the_endpoints);
    RUN_TEST(test_arpeggiator_octave_range_and_reset);
    RUN_TEST(test_arpeggiator_releases_when_the_chord_is_lifted);
    RUN_TEST(test_arpeggiator_gate_length_releases_early);
    RUN_TEST(test_transpose_hangs_nothing);
    RUN_TEST(test_note_priority_hangs_nothing);
    RUN_TEST(test_velocity_curve_hangs_nothing);
    RUN_TEST(test_chord_hangs_nothing);
    RUN_TEST(test_quantise_hangs_nothing);
    RUN_TEST(test_probability_hangs_nothing);
    RUN_TEST(test_arpeggiator_hangs_nothing_in_any_mode);
    return UNITY_END();
}
