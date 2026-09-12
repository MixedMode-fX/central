#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/held_notes.h"
#include "midi/sounding_notes.h"
#include "midi/scale.h"
#include "midi/global_scale.h"
#include "midi/note_event.h"
#include "algorithm/midi/transpose.h"
#include "algorithm/midi/note_priority.h"
#include "algorithm/midi/velocity_curve.h"
#include "algorithm/midi/chord.h"
#include "algorithm/midi/note_quantise.h"
#include "algorithm/midi/probability.h"
#include "algorithm/midi/arpeggiator.h"
#include "algorithm/midi/midi_to_cv.h"

void setUp() {}
// The module's key is process-wide state (midi/global_scale.h), so every test
// gets it back the way it found it: chromatic, which is what a module with no
// key set is in.
void tearDown() { global_scale::set(SCALE_CHROMATIC, 0); }

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
// The one place that answers "which held note wins", and the one lookup that
// finds a note once it has been chosen. Two algorithms lean on these, so they
// are tested here rather than only through whichever of them happens to run.
static void test_held_notes_answers_the_priority_rules_and_finds_a_note() {
    HeldNotes held;
    HeldNote evicted = {0, 0, 0};
    bool did = false;

    // Nothing held has one answer, whatever the rule.
    for (uint8_t rule = 0; rule < NOTE_PRIORITY_RULES; rule++) {
        TEST_ASSERT_EQUAL(HeldNotes::NONE, held.winner((NotePriorityRule)rule));
    }

    held.add(60, 100, 1, evicted, did);
    held.add(67, 90, 2, evicted, did);
    held.add(64, 80, 3, evicted, did);
    TEST_ASSERT_EQUAL(60, held.winner(NOTE_PRIORITY_LOWEST));
    TEST_ASSERT_EQUAL(67, held.winner(NOTE_PRIORITY_HIGHEST));
    TEST_ASSERT_EQUAL(64, held.winner(NOTE_PRIORITY_LATEST));

    // A chosen note comes back with everything it arrived with.
    const HeldNote* found = held.find(67);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL(90, found->velocity);
    TEST_ASSERT_EQUAL(2, found->channel);
    TEST_ASSERT_NULL(held.find(72));
    TEST_ASSERT_TRUE(held.contains(64));
    TEST_ASSERT_FALSE(held.contains(72));

    // And both parameter numberings land on the rule they name.
    TEST_ASSERT_EQUAL(held.winner(NOTE_PRIORITY_HIGHEST),
                      held.winner((NotePriorityRule)NotePriority::PRIORITY_HIGH));
    TEST_ASSERT_EQUAL(held.winner(NOTE_PRIORITY_HIGHEST),
                      held.winner((NotePriorityRule)(MidiToCv::PRIORITY_HIGHEST - 1)));
}

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

static void test_chord_emits_every_voice_and_releases_all_of_it() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_CHORD);
    c.in_bus[0] = 0; c.out_bus[0] = 1;            // a triad, the default quality
    Chord node(c);

    global_scale::set(SCALE_CHROMATIC, 0);        // no key: the major triad
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
// NoteQuantise
// ---------------------------------------------------------------------------

static void test_note_quantise_snaps_and_releases_what_it_sent() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_NOTE_QUANTISE);
    c.in_bus[0] = 0; c.in_bus[1] = NO_BUS; c.out_bus[0] = 1;
    c.params[0] = SCALE_MAJOR;
    c.params[1] = 0;                              // C
    NoteQuantise node(c);

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

static void test_note_quantise_takes_its_root_from_a_bus() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_NOTE_QUANTISE);
    c.in_bus[0] = 0; c.in_bus[1] = 2; c.out_bus[0] = 1;
    c.params[0] = SCALE_MAJOR;
    NoteQuantise node(c);

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
// The module's scale (midi/global_scale.h)
// ---------------------------------------------------------------------------

// An algorithm that named no scale follows the module's; one that named a
// scale keeps it, and takes its own root with it.
static void test_the_global_scale_is_the_default_and_an_override_wins() {
    global_scale::set(SCALE_MAJOR, 2);                 // D major
    TEST_ASSERT_EQUAL_HEX16(scale_mask(SCALE_MAJOR), global_scale::mask());
    TEST_ASSERT_EQUAL(2, global_scale::root());

    TEST_ASSERT_TRUE(global_scale::follows(SCALE_GLOBAL));
    TEST_ASSERT_FALSE(global_scale::follows(SCALE_BLUES));
    TEST_ASSERT_EQUAL_HEX16(scale_mask(SCALE_MAJOR), global_scale::resolve_id(SCALE_GLOBAL));
    TEST_ASSERT_EQUAL_HEX16(scale_mask(SCALE_BLUES), global_scale::resolve_id(SCALE_BLUES));
    TEST_ASSERT_EQUAL(2, global_scale::resolve_root(SCALE_GLOBAL, 7));
    TEST_ASSERT_EQUAL(7, global_scale::resolve_root(SCALE_BLUES, 7));

    // Chromatic is a scale like any other, and is what the module is in
    // until a key is set - which is why a patch written before the setting
    // existed plays what it always did.
    global_scale::set(SCALE_CHROMATIC, 0);
    TEST_ASSERT_EQUAL_HEX16(0x0FFF, global_scale::resolve_id(SCALE_GLOBAL));
    // The global scale cannot follow itself.
    global_scale::set(SCALE_GLOBAL, 5);
    TEST_ASSERT_EQUAL(SCALE_CHROMATIC, global_scale::id());
    TEST_ASSERT_EQUAL_HEX16(0x0FFF, global_scale::mask());
}

// A scale and a pitch class say which notes and which is home; they cannot
// say *where* home is. The register is the third part of the key, it is unset
// by default, and unset means every node keeps the root it stored - which is
// what makes a patch written before it existed play what it always did.
static void test_the_key_can_name_a_register() {
    global_scale::set(SCALE_NATURAL_MINOR, 9);            // A minor, no register
    TEST_ASSERT_EQUAL(0, global_scale::octave());
    TEST_ASSERT_EQUAL(global_scale::NO_ROOT_NOTE, global_scale::root_note());

    global_scale::set(SCALE_NATURAL_MINOR, 9, 3);         // A minor, at octave 3
    TEST_ASSERT_EQUAL(3, global_scale::octave());
    TEST_ASSERT_EQUAL(45, global_scale::root_note());  // 3 x 12 + 9

    // The top octave cannot hold every pitch class, and a key that silently
    // became a different note would be worse than one an octave lower.
    global_scale::set(SCALE_NATURAL_MINOR, 11, 10);
    TEST_ASSERT_EQUAL(119, global_scale::root_note());

    // Out of range is clamped rather than wrapped.
    global_scale::set(SCALE_NATURAL_MINOR, 0, 200);
    TEST_ASSERT_EQUAL(10, global_scale::octave());
}

// The two resolvers, which is where the register actually reaches a node.
// Which notes and which of them is home are two questions now: a node that
// names a scale of its own still plays the key's root, and only its `key`
// parameter takes it out of the key.
static void test_the_register_resolves_for_both_kinds_of_root() {
    // No register: a tonic takes the key's pitch class in its own octave, and
    // a pattern's anchor is left exactly where it was.
    global_scale::set(SCALE_NATURAL_MINOR, 9);
    TEST_ASSERT_EQUAL(57, global_scale::resolve_tonic(global_scale::KEY_FOLLOW, 48, 48));
    TEST_ASSERT_EQUAL(48, global_scale::resolve_anchor(global_scale::KEY_FOLLOW, 48, 48));

    // With one, both take it.
    global_scale::set(SCALE_NATURAL_MINOR, 9, 2);
    TEST_ASSERT_EQUAL(33, global_scale::resolve_tonic(global_scale::KEY_FOLLOW, 48, 48));
    TEST_ASSERT_EQUAL(33, global_scale::resolve_anchor(global_scale::KEY_FOLLOW, 48, 48));

    // A node moved off the register its root parameter holds by default keeps
    // that distance from the key rather than being flattened onto it: one key
    // for the patch, one octave per node.
    TEST_ASSERT_EQUAL(21, global_scale::resolve_tonic(global_scale::KEY_FOLLOW, 36, 48));
    TEST_ASSERT_EQUAL(45, global_scale::resolve_anchor(global_scale::KEY_FOLLOW, 60, 48));

    // `own` is the opt-out, and the only one. Naming a scale is not one.
    TEST_ASSERT_EQUAL(48, global_scale::resolve_tonic(global_scale::KEY_OWN, 48, 48));
    TEST_ASSERT_EQUAL(48, global_scale::resolve_anchor(global_scale::KEY_OWN, 48, 48));
    TEST_ASSERT_EQUAL(7, global_scale::resolve_root(global_scale::KEY_OWN, 7));
    TEST_ASSERT_EQUAL(9, global_scale::resolve_root(global_scale::KEY_FOLLOW, 7));
}

static void test_note_quantise_follows_the_module_scale_until_it_names_one() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_NOTE_QUANTISE);
    c.in_bus[0] = 0; c.out_bus[0] = 1;                 // scale left at 0: the module's
    NoteQuantise node(c);

    global_scale::set(SCALE_MAJOR, 2);                 // D major
    bus.note_write(0, on(60));                         // C is not in it -> C#
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(61, out[0].data1);
    TEST_ASSERT_EQUAL(2, node.active_root());

    // Naming a scale says which notes, and nothing about which of them is
    // home: the key's root still is. D pentatonic major has no C# to reach,
    // so C lands on the B below it.
    bus.note_write(0, off(60));
    run_pass(bus, node, 1);
    node.set_scale(SCALE_PENTATONIC_MAJOR);
    TEST_ASSERT_EQUAL(2, node.active_root());
    bus.note_write(0, on(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(59, out[0].data1);               // B, the nearest tone it has

    // `key` is what leaves the key, and it leaves only the root behind: the
    // node's own root parameter, C, is what the pentatonic is built on now.
    bus.note_write(0, off(62));
    run_pass(bus, node, 1);
    node.set_key(global_scale::KEY_OWN);
    TEST_ASSERT_EQUAL(0, node.active_root());
    bus.note_write(0, on(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
}

// A quality is scale steps, so one setting is a triad on every degree.
static void test_chord_voices_its_quality_in_the_scale() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_CHORD);
    c.in_bus[0] = 0; c.out_bus[0] = 1;
    c.params[Chord::P_QUALITY] = Chord::QUALITY_TRIAD;
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);                 // C major
    bus.note_write(0, on(60));                         // C E G
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(64, out[1].data1);
    TEST_ASSERT_EQUAL(67, out[2].data1);
    bus.note_write(0, off(60));
    run_pass(bus, node, 1);

    bus.note_write(0, on(62));                         // D F A: the same 0 2 4
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(62, out[0].data1);
    TEST_ASSERT_EQUAL(65, out[1].data1);
    TEST_ASSERT_EQUAL(69, out[2].data1);
    bus.note_write(0, off(62));
    run_pass(bus, node, 1);

    // A note outside the key is snapped into it, so the chord is in key even
    // when the playing is not.
    bus.note_write(0, on(61));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(62, out[0].data1);
    TEST_ASSERT_EQUAL(65, out[1].data1);
    TEST_ASSERT_EQUAL(69, out[2].data1);
}

// Chromatic on the node is the escape hatch: a chord of fixed semitones,
// whatever key the module is in. A triad of twelve equal steps would be a
// cluster, so a chromatic scale plays the quality's own shape instead.
static void test_chord_in_the_chromatic_scale_is_fixed_semitones() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_CHORD);
    c.in_bus[0] = 0; c.out_bus[0] = 1;
    c.params[Chord::P_SCALE] = SCALE_CHROMATIC;        // a major triad in semitones
    Chord node(c);

    global_scale::set(SCALE_PENTATONIC_MINOR, 3);
    bus.note_write(0, on(61));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(61, out[0].data1);
    TEST_ASSERT_EQUAL(65, out[1].data1);
    TEST_ASSERT_EQUAL(68, out[2].data1);
}

// ---------------------------------------------------------------------------
// Chord with no note inlet: it plays itself
// ---------------------------------------------------------------------------

static NodeConfig free_chord(bool with_root_inlet) {
    NodeConfig c = node_config(ALGO_CHORD);
    c.in_bus[0] = NO_BUS;                              // nothing plays it
    if (with_root_inlet) c.in_bus[1] = 0;
    c.out_bus[0] = 1;                                  // a triad, the default quality
    return c;
}

// The acceptance test for "set the notes and let it run": no keyboard, no
// clock, no gate - and a chord sounding.
static void test_a_chord_with_no_note_inlet_plays_itself_and_holds() {
    BusManager bus;
    NodeConfig c = free_chord(false);
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);                 // C major
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);               // middle C, the default octave
    TEST_ASSERT_EQUAL(64, out[1].data1);
    TEST_ASSERT_EQUAL(67, out[2].data1);
    for (const MidiEvent& e : out) TEST_ASSERT_TRUE(is_note_on(e));

    // Held, not repeated: a downstream arpeggiator sees one chord, not a
    // note-on every pass.
    for (uint8_t i = 0; i < 20; i++){
        out = run_pass(bus, node, 1);
        TEST_ASSERT_EQUAL(0, out.size());
    }
    TEST_ASSERT_EQUAL(3, node.sounding_count());
}

// The octave places it, and the velocity is the one nobody played.
static void test_a_self_playing_chord_takes_its_octave_and_velocity() {
    BusManager bus;
    NodeConfig c = free_chord(false);
    c.params[Chord::P_OCTAVE] = 3;
    c.params[Chord::P_VELOCITY] = 64;
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(36, out[0].data1);
    TEST_ASSERT_EQUAL(40, out[1].data1);
    TEST_ASSERT_EQUAL(43, out[2].data1);
    for (const MidiEvent& e : out) TEST_ASSERT_EQUAL(64, e.data2);
}

// A named quality is the intervals nobody should have to type. In C major on
// the tonic, `7th` is C E G B - four voices from one parameter, and the
// seventh is major because the fourth degree above C in this key is B.
static void test_chord_quality_names_a_stack_of_scale_steps() {
    BusManager bus;
    NodeConfig c = free_chord(false);
    c.params[Chord::P_QUALITY] = Chord::QUALITY_SEVENTH;
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(4, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(64, out[1].data1);
    TEST_ASSERT_EQUAL(67, out[2].data1);
    TEST_ASSERT_EQUAL(71, out[3].data1);               // B: 2 4 6 are scale steps
}

// The same stack on the fifth degree is a dominant seventh and on the second a
// minor seventh, with nothing anywhere naming either: the quality is degrees,
// so the key decides what they sound like.
static void test_chord_quality_takes_its_flavour_from_the_degree() {
    BusManager bus;
    NodeConfig c = free_chord(true);
    c.params[Chord::P_QUALITY] = Chord::QUALITY_SEVENTH;
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);
    run_pass(bus, node, 1);

    bus.note_write(0, on(67));                         // G, the fifth degree
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(8, out.size());                  // four off, four on
    TEST_ASSERT_EQUAL(67, out[4].data1);
    TEST_ASSERT_EQUAL(71, out[5].data1);
    TEST_ASSERT_EQUAL(74, out[6].data1);
    TEST_ASSERT_EQUAL(77, out[7].data1);               // F natural: G7, not Gmaj7
}

// One self-playing chord in C major, sounded once, so a voicing and an
// inversion can be read straight off the notes it emitted.
static std::vector<MidiEvent> chord_voices(uint8_t quality, uint8_t voicing, uint8_t inversion) {
    NodeConfig c = free_chord(false);
    c.params[Chord::P_QUALITY] = quality;
    c.params[Chord::P_VOICING] = voicing;
    c.params[Chord::P_INVERSION] = inversion;
    Chord node(c);
    BusManager bus;
    return run_pass(bus, node, 1);
}

// `inversion` moves the lowest voices up an octave, so what is in the bass is
// a voice of the chord rather than always its root.
static void test_chord_inversion_moves_the_bass() {
    global_scale::set(SCALE_MAJOR, 0);                  // C major

    std::vector<MidiEvent> out = chord_voices(Chord::QUALITY_TRIAD, Chord::VOICING_CLOSE, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(64, out[0].data1);                // E G C: the third in the bass
    TEST_ASSERT_EQUAL(67, out[1].data1);
    TEST_ASSERT_EQUAL(72, out[2].data1);

    out = chord_voices(Chord::QUALITY_TRIAD, Chord::VOICING_CLOSE, 2);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(67, out[0].data1);                // G C E
    TEST_ASSERT_EQUAL(72, out[1].data1);
    TEST_ASSERT_EQUAL(76, out[2].data1);

    // More inversions than the chord has voices is the highest one it has.
    out = chord_voices(Chord::QUALITY_TRIAD, Chord::VOICING_CLOSE, Chord::MAX_INVERSION);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(67, out[0].data1);
}

// `voicing` is the shape of the stack: the same notes, further apart.
static void test_chord_voicing_opens_the_stack() {
    global_scale::set(SCALE_MAJOR, 0);                  // C major

    // Open position: the middle voice up an octave, the bass where it was.
    std::vector<MidiEvent> out = chord_voices(Chord::QUALITY_TRIAD, Chord::VOICING_OPEN, 0);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);                // C G E
    TEST_ASSERT_EQUAL(67, out[1].data1);
    TEST_ASSERT_EQUAL(76, out[2].data1);

    // Drop 2 on a seventh: the second voice from the top, an octave down.
    out = chord_voices(Chord::QUALITY_SEVENTH, Chord::VOICING_DROP2, 0);
    TEST_ASSERT_EQUAL(4, out.size());
    TEST_ASSERT_EQUAL(55, out[0].data1);                // G below the C: G C E B
    TEST_ASSERT_EQUAL(60, out[1].data1);
    TEST_ASSERT_EQUAL(64, out[2].data1);
    TEST_ASSERT_EQUAL(71, out[3].data1);

    // Drop 3 takes the third voice from the top instead.
    out = chord_voices(Chord::QUALITY_SEVENTH, Chord::VOICING_DROP3, 0);
    TEST_ASSERT_EQUAL(4, out.size());
    TEST_ASSERT_EQUAL(52, out[0].data1);                // E below the C: E C G B
    TEST_ASSERT_EQUAL(60, out[1].data1);

    // Wide puts an octave under every voice, so a triad covers three.
    out = chord_voices(Chord::QUALITY_TRIAD, Chord::VOICING_WIDE, 0);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(76, out[1].data1);
    TEST_ASSERT_EQUAL(91, out[2].data1);

    // The inversion is applied first, so a voicing opens out the inversion
    // and not the root position it came from.
    out = chord_voices(Chord::QUALITY_TRIAD, Chord::VOICING_OPEN, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(64, out[0].data1);                // E C G, still on the third
    TEST_ASSERT_EQUAL(72, out[1].data1);
    TEST_ASSERT_EQUAL(79, out[2].data1);
}

// `Harmony` repeats a degree whenever its style or its gravity says so, and a
// progression where one chord of the phrase does not sound is a hole in it.
// Held is still the default - this is the switch that says otherwise.
static void test_a_repeated_root_re_strikes_only_when_asked() {
    BusManager bus;
    NodeConfig c = free_chord(true);
    c.params[Chord::P_RETRIGGER] = 1;
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);
    run_pass(bus, node, 1);

    bus.note_write(0, on(62));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(6, out.size());

    // The same root again: three note-offs and the same three notes back.
    bus.note_write(0, on(62));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(6, out.size());
    for (uint8_t i = 0; i < 3; i++) TEST_ASSERT_TRUE(is_note_off(out[i]));
    TEST_ASSERT_EQUAL(62, out[3].data1);
    TEST_ASSERT_EQUAL(65, out[4].data1);
    TEST_ASSERT_EQUAL(69, out[5].data1);
    TEST_ASSERT_EQUAL(3, node.sounding_count());

    // And nothing is left sounding when it goes quiet.
    node.silence(bus);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

// A self-playing chord sits in its own octave until the key names one, and
// then it sits where the module says home is. The root inlet still outranks
// both, because a cable is the most explicit thing a user can say.
static void test_a_self_playing_chord_follows_the_key_register() {
    BusManager bus;
    NodeConfig c = free_chord(true);
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);                 // C major, no register
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);               // its own octave, middle C

    global_scale::set(SCALE_MAJOR, 0, 3);              // and now the key has one
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(6, out.size());
    for (uint8_t i = 0; i < 3; i++) TEST_ASSERT_TRUE(is_note_off(out[i]));
    TEST_ASSERT_EQUAL(36, out[3].data1);               // C2: 3 x 12
    TEST_ASSERT_EQUAL(40, out[4].data1);
    TEST_ASSERT_EQUAL(43, out[5].data1);

    // A root on the inlet is the note to play, register or no register.
    bus.note_write(0, on(67));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(6, out.size());
    TEST_ASSERT_EQUAL(67, out[3].data1);

    // An octave below the one it sits in by default is an octave below the
    // key, not a chord flattened onto it.
    NodeConfig lower = free_chord(false);
    lower.params[Chord::P_OCTAVE] = Chord::DEFAULT_OCTAVE - 1;
    Chord below(lower);
    BusManager third;
    out = run_pass(third, below, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(24, out[0].data1);               // C1, an octave under C2

    // And a chord told to keep its own key is not moved at all.
    NodeConfig own = free_chord(false);
    own.params[Chord::P_SCALE] = SCALE_MAJOR;
    own.params[Chord::P_ROOT] = 0;
    own.params[Chord::P_KEY] = global_scale::KEY_OWN;
    Chord fixed(own);
    BusManager other;
    out = run_pass(other, fixed, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
}

// A sequencer on the root inlet is what plays a self-playing chord: the whole
// note, and the chords stay in the key rather than dragging it around. In C
// major a root of D is D minor, which is what "diatonic" means and what a
// fixed semitone stack would have got wrong.
static void test_a_sequenced_root_walks_a_self_playing_chord_through_the_key() {
    BusManager bus;
    NodeConfig c = free_chord(true);
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);
    run_pass(bus, node, 1);                            // the tonic triad first

    bus.note_write(0, on(62));                         // D, from a sequencer
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(6, out.size());
    for (uint8_t i = 0; i < 3; i++) TEST_ASSERT_TRUE(is_note_off(out[i]));
    TEST_ASSERT_EQUAL(62, out[3].data1);
    TEST_ASSERT_EQUAL(65, out[4].data1);               // F, not F#
    TEST_ASSERT_EQUAL(69, out[5].data1);
    TEST_ASSERT_EQUAL(3, node.sounding_count());

    // The same root again changes nothing: the chord is already there.
    bus.note_write(0, on(62));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(0, out.size());

    // An octave lower is a different chord, and the old one is released.
    bus.note_write(0, on(50));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(6, out.size());
    TEST_ASSERT_EQUAL(50, out[3].data1);
    TEST_ASSERT_EQUAL(53, out[4].data1);
    TEST_ASSERT_EQUAL(57, out[5].data1);
}

// Editing the voicing of a chord that is already droning has to be audible,
// and cannot strand the notes it replaces.
static void test_editing_a_self_playing_chord_re_voices_it() {
    BusManager bus;
    NodeConfig c = free_chord(false);
    Chord node(c);

    global_scale::set(SCALE_MAJOR, 0);
    NoteBalance balance;
    balance.observe(run_pass(bus, node, 1));

    TEST_ASSERT_TRUE(node.set_param(Chord::P_QUALITY, Chord::QUALITY_SEVENTH));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    balance.observe(out);
    TEST_ASSERT_EQUAL(7, out.size());                  // three off, four on
    TEST_ASSERT_EQUAL(71, out[6].data1);
    TEST_ASSERT_EQUAL(4, balance.total());

    // And the key moving under it moves the chord, with nothing left behind.
    global_scale::set(SCALE_NATURAL_MINOR, 0);
    out = run_pass(bus, node, 1);
    balance.observe(out);
    TEST_ASSERT_EQUAL(8, out.size());
    TEST_ASSERT_EQUAL(63, out[5].data1);               // E flat now
    TEST_ASSERT_EQUAL(4, balance.total());

    // The patch swap takes it down: a drone is still a note somebody owns.
    node.silence(bus);
    bus.swap();
    std::vector<MidiEvent> tail;
    const uint8_t n = bus.note_count(1);
    for (uint8_t i = 0; i < n; i++) tail.push_back(bus.note_read(1, i));
    balance.observe(tail);
    TEST_ASSERT_EQUAL(0, balance.total());
    TEST_ASSERT_FALSE(balance.went_negative);
}

// The whole point of the pair: a chord nobody is holding, arpeggiated by a
// clock. No MIDI input, no gate input, and a figure running.
static void test_a_self_playing_chord_feeds_an_arpeggiator() {
    BusManager bus;
    NodeConfig cc = free_chord(false);
    Chord chord(cc);
    NodeConfig ac = node_config(ALGO_ARPEGGIATOR);
    ac.in_bus[0] = 1;                                  // the chord bus
    ac.in_bus[1] = 0;                                  // advance
    ac.out_bus[0] = 2;
    Arpeggiator arp(ac);

    global_scale::set(SCALE_MAJOR, 0);
    std::vector<uint8_t> played;
    // One swap a pass, as the master runs it: the nodes read what was written
    // last time and write what the next pass will read.
    for (uint32_t pass = 0; pass < 16; pass++){
        chord.process(bus, pass * 1000u);
        arp.process(bus, pass * 1000u);
        bus.gate_write(0, (pass % 2) == 0);             // a clock, and nothing else
        bus.swap();
        const uint8_t n = bus.note_count(2);
        for (uint8_t i = 0; i < n; i++){
            const MidiEvent e = bus.note_read(2, i);
            if (is_note_on(e)) played.push_back(e.data1);
        }
    }
    TEST_ASSERT_TRUE(played.size() >= 3);
    TEST_ASSERT_EQUAL(60, played[0]);
    TEST_ASSERT_EQUAL(64, played[1]);
    TEST_ASSERT_EQUAL(67, played[2]);
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

// The single note-on in a pass, or NONE. Every arpeggiator step is one note.
static uint8_t note_played(const std::vector<MidiEvent>& out) {
    uint8_t played = HeldNotes::NONE;
    for (const MidiEvent& e : out) if (is_note_on(e)) played = e.data1;
    return played;
}

// One edge, one step, with nobody touching the keyboard: the point of hold.
static void test_arpeggiator_hold_keeps_the_figure_after_the_keys_are_lifted() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.out_bus[0] = 1;
    c.params[4] = 1;                                   // hold
    Arpeggiator node(c);

    hold_triad(bus, 0);
    run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, node.keys_down());

    bus.note_write(0, off(60));
    bus.note_write(0, off(64));
    bus.note_write(0, off(67));
    run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(0, node.keys_down());
    TEST_ASSERT_EQUAL(3, node.held_count());           // the figure survives
    TEST_ASSERT_TRUE(node.latched());

    const uint8_t expected[3] = {60, 64, 67};
    for (uint8_t step = 0; step < 3; step++) {
        bus.gate_write(0, true);
        TEST_ASSERT_EQUAL(expected[step], note_played(run_pass(bus, node, 1)));
        bus.gate_write(0, false);
        run_pass(bus, node, 1);
    }
}

// The first key of a new chord replaces the latched one instead of adding to
// it - otherwise every fumbled change leaves a note in the figure for ever.
static void test_arpeggiator_hold_replaces_the_chord_rather_than_adding_to_it() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.out_bus[0] = 1;
    c.params[4] = 1;
    Arpeggiator node(c);

    hold_triad(bus, 0);
    run_pass(bus, node, 1);
    bus.gate_write(0, true);
    NoteBalance balance;
    balance.observe(run_pass(bus, node, 1));
    bus.gate_write(0, false);
    balance.observe(run_pass(bus, node, 1));
    bus.note_write(0, off(60));
    bus.note_write(0, off(64));
    bus.note_write(0, off(67));
    balance.observe(run_pass(bus, node, 1));

    bus.note_write(0, on(72));
    balance.observe(run_pass(bus, node, 1));
    TEST_ASSERT_EQUAL(1, node.held_count());           // the new chord, not four notes
    TEST_ASSERT_EQUAL(0, balance.total());             // and the latched note went with it

    // Adding to a held chord is still possible: keep one key down.
    bus.note_write(0, on(76));
    run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, node.held_count());
    bus.gate_write(0, true);
    TEST_ASSERT_EQUAL(72, note_played(run_pass(bus, node, 1)));
}

// Hold coming off keeps what is still under the fingers and drops the rest,
// so lifting the latch does not cut the notes being played.
static void test_arpeggiator_hold_off_drops_only_what_no_key_holds() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.out_bus[0] = 1;
    c.params[4] = 1;
    Arpeggiator node(c);

    bus.note_write(0, on(60));
    bus.note_write(0, on(64));
    run_pass(bus, node, 1);
    bus.note_write(0, off(64));                        // one finger up, one down
    run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, node.held_count());
    TEST_ASSERT_EQUAL(1, node.keys_down());

    TEST_ASSERT_TRUE(node.set_param(4, 0));            // hold off
    run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, node.held_count());
    TEST_ASSERT_FALSE(node.latched());
    bus.gate_write(0, true);
    TEST_ASSERT_EQUAL(60, note_played(run_pass(bus, node, 1)));
}

// The inlet is the parameter: a footswitch latches, and letting it go with no
// key down stands the arpeggiator down without leaving a note sounding.
static void test_arpeggiator_hold_inlet_latches_like_the_parameter() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.in_bus[3] = 2; c.out_bus[0] = 1;
    Arpeggiator node(c);

    NoteBalance balance;
    bus.gate_write(2, true);                           // the switch goes down
    hold_triad(bus, 0);
    balance.observe(run_pass(bus, node, 1));
    bus.gate_write(2, true);
    bus.note_write(0, off(60));
    bus.note_write(0, off(64));
    bus.note_write(0, off(67));
    balance.observe(run_pass(bus, node, 1));
    TEST_ASSERT_EQUAL(3, node.held_count());

    bus.gate_write(2, true);
    bus.gate_write(0, true);
    balance.observe(run_pass(bus, node, 1));
    TEST_ASSERT_EQUAL(1, balance.total());

    bus.gate_write(2, false);                          // and comes back up
    bus.gate_write(0, false);
    balance.observe(run_pass(bus, node, 1));
    TEST_ASSERT_EQUAL(0, node.held_count());
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
// MidiToCV: a note stream leaving as pitch, gate, velocity, mod and a trigger
// ---------------------------------------------------------------------------

// The buses the rig patches, one per outlet. Pitch and gate share index 0 in
// their own domains, which is legal and is what a real patch looks like.
static const uint8_t CV_PITCH_BUS = 0;
static const uint8_t CV_GATE_BUS = 0;
static const uint8_t CV_VELOCITY_BUS = 1;
static const uint8_t CV_MOD_BUS = 2;
static const uint8_t CV_TRIGGER_BUS = 1;

struct CvVoice {
    int16_t pitch;
    bool gate;
    int16_t velocity;
    int16_t mod;
    bool trigger;
};

static NodeConfig cv_config() {
    NodeConfig c = node_config(ALGO_MIDI_TO_CV);
    c.in_bus[0] = 0;
    c.out_bus[0] = CV_PITCH_BUS;
    c.out_bus[1] = CV_GATE_BUS;
    c.out_bus[2] = CV_VELOCITY_BUS;
    c.out_bus[3] = CV_MOD_BUS;
    c.out_bus[4] = CV_TRIGGER_BUS;
    return c;
}

// One pass, and what the five outlets carried when it was over.
static CvVoice run_cv_pass(BusManager& bus, MidiToCv& node, uint32_t now_us = 0) {
    bus.swap();
    node.process(bus, now_us);
    bus.swap();
    return CvVoice{bus.cv_read(CV_PITCH_BUS), bus.gate_read(CV_GATE_BUS),
                   bus.cv_read(CV_VELOCITY_BUS), bus.cv_read(CV_MOD_BUS),
                   bus.gate_read(CV_TRIGGER_BUS)};
}

// What the node should put on the pitch bus for a note, at the default range
// of ten octaves from C2: full scale over 120 semitones, so a semitone is
// 4096/120 and the assertions below are written in semitones rather than in
// bus units nobody would recognise.
static int16_t semitones_above_base(int32_t semitones, uint8_t range = MidiToCv::DEFAULT_RANGE) {
    const int32_t per_semitone = ((int32_t)CV_FULL << 8) / (12 * (int32_t)range);
    return (int16_t)cv_clamp_unipolar((semitones * per_semitone) >> 8);
}

static MidiEvent bend_to(uint16_t value, uint8_t channel = 1) {
    return MidiEvent{MIDI_PITCH_BEND, channel, (uint8_t)(value & 0x7F), (uint8_t)((value >> 7) & 0x7F)};
}
static MidiEvent cc(uint8_t controller, uint8_t value, uint8_t channel = 1) {
    return MidiEvent{MIDI_CONTROL_CHANGE, channel, controller, value};
}

static void test_midi_to_cv_pitch_is_an_octave_per_range_step() {
    BusManager bus;
    NodeConfig c = cv_config();
    MidiToCv node(c);

    // The base note sits at the bottom of the range.
    bus.note_write(0, on(MidiToCv::DEFAULT_BASE));
    TEST_ASSERT_EQUAL_INT16(0, run_cv_pass(bus, node).pitch);

    // An octave up is a tenth of full scale, at ten octaves of range.
    bus.note_write(0, off(MidiToCv::DEFAULT_BASE));
    bus.note_write(0, on(MidiToCv::DEFAULT_BASE + 12));
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(12), run_cv_pass(bus, node).pitch);
    TEST_ASSERT_INT16_WITHIN(1, CV_FULL / 10, run_cv_pass(bus, node).pitch);

    // Five octaves up is half of it.
    bus.note_write(0, off(MidiToCv::DEFAULT_BASE + 12));
    bus.note_write(0, on(MidiToCv::DEFAULT_BASE + 60));
    TEST_ASSERT_INT16_WITHIN(1, CV_HALF, run_cv_pass(bus, node).pitch);

    // Halving the range doubles the interval: the same note, twice as high on
    // the bus, and it moves under the held note rather than at the next one.
    TEST_ASSERT_TRUE(node.set_param(1, 5));
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(60, 5), run_cv_pass(bus, node).pitch);
}

static void test_a_note_below_the_base_clamps_at_the_bottom() {
    BusManager bus;
    NodeConfig c = cv_config();
    MidiToCv node(c);

    bus.note_write(0, on(MidiToCv::DEFAULT_BASE - 12));
    const CvVoice under = run_cv_pass(bus, node);
    TEST_ASSERT_EQUAL_INT16(0, under.pitch);      // flat, never wrapped to the top
    TEST_ASSERT_TRUE(under.gate);                 // and still a note

    // Moving the base down puts it back in the range.
    TEST_ASSERT_TRUE(node.set_param(2, MidiToCv::DEFAULT_BASE - 24));
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(12), run_cv_pass(bus, node).pitch);
}

static void test_the_gate_is_up_for_as_long_as_a_key_is_held() {
    BusManager bus;
    NodeConfig c = cv_config();
    MidiToCv node(c);

    TEST_ASSERT_FALSE(run_cv_pass(bus, node).gate);
    bus.note_write(0, on(60));
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 1000).gate);
    for (uint32_t t = 2000; t < 100000; t += 1000) {
        TEST_ASSERT_TRUE(run_cv_pass(bus, node, t).gate);
    }
    bus.note_write(0, off(60));
    TEST_ASSERT_FALSE(run_cv_pass(bus, node, 101000).gate);

    // The pitch outlives the gate: an envelope in its release must not be
    // dragged to another note by the key coming up.
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(60 - MidiToCv::DEFAULT_BASE),
                            run_cv_pass(bus, node, 102000).pitch);
}

static void test_the_priority_decides_which_held_note_the_voice_takes() {
    const uint8_t modes[3] = {MidiToCv::PRIORITY_LOWEST, MidiToCv::PRIORITY_HIGHEST,
                              MidiToCv::PRIORITY_LATEST};
    const uint8_t expected[3] = {60, 67, 64};        // played 60, then 67 and 64
    for (uint8_t m = 0; m < 3; m++) {
        BusManager bus;
        NodeConfig c = cv_config();
        c.params[0] = modes[m];
        MidiToCv node(c);

        bus.note_write(0, on(60));
        run_cv_pass(bus, node);
        TEST_ASSERT_EQUAL(60, node.voice());

        bus.note_write(0, on(67));
        bus.note_write(0, on(64));
        const CvVoice v = run_cv_pass(bus, node, 1000);
        TEST_ASSERT_EQUAL(expected[m], node.voice());
        TEST_ASSERT_EQUAL_INT16(semitones_above_base((int32_t)expected[m] - MidiToCv::DEFAULT_BASE),
                                v.pitch);
        TEST_ASSERT_TRUE(v.gate);
    }
}

static void test_releasing_a_key_hands_the_voice_back_without_a_new_attack() {
    BusManager bus;
    NodeConfig c = cv_config();
    c.params[0] = MidiToCv::PRIORITY_LATEST;
    MidiToCv node(c);

    bus.note_write(0, on(60));
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 1000).trigger);          // the attack
    bus.note_write(0, on(64));
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 20000).trigger);         // the second attack

    // The upper key comes up. The voice goes back to the note still held -
    // and that is a legato move, not a strike, so nothing retriggers.
    bus.note_write(0, off(64));
    const CvVoice back = run_cv_pass(bus, node, 40000);
    TEST_ASSERT_EQUAL(60, node.voice());
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(60 - MidiToCv::DEFAULT_BASE), back.pitch);
    TEST_ASSERT_TRUE(back.gate);
    TEST_ASSERT_FALSE(back.trigger);
}

static void test_the_trigger_is_a_fixed_width_pulse_on_every_attack() {
    BusManager bus;
    NodeConfig c = cv_config();
    c.params[5] = 10;                                  // 10 ms
    MidiToCv node(c);

    TEST_ASSERT_FALSE(run_cv_pass(bus, node, 0).trigger);
    bus.note_write(0, on(60));
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 1000).trigger);
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 9000).trigger);
    TEST_ASSERT_FALSE(run_cv_pass(bus, node, 12000).trigger);
    // ... while the gate is still up: the two outlets answer different
    // questions and only one of them has ended.
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 13000).gate);

    // A key struck under the one already down fires it again.
    bus.note_write(0, on(72));
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 14000).trigger);
}

static void test_retrigger_mode_drops_the_gate_for_one_pass() {
    BusManager bus;
    NodeConfig c = cv_config();
    c.params[0] = MidiToCv::PRIORITY_LATEST;
    c.params[4] = MidiToCv::GATE_RETRIGGER;
    MidiToCv node(c);

    bus.note_write(0, on(60));
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 1000).gate);       // nothing to re-gate yet
    bus.note_write(0, on(64));
    TEST_ASSERT_FALSE(run_cv_pass(bus, node, 2000).gate);      // the re-gate
    TEST_ASSERT_TRUE(run_cv_pass(bus, node, 3000).gate);       // and back up, one pass later
    TEST_ASSERT_EQUAL(64, node.voice());

    // In legato - the default - the same two keys never drop it.
    BusManager legato_bus;
    NodeConfig legato_config = cv_config();
    legato_config.params[0] = MidiToCv::PRIORITY_LATEST;
    MidiToCv legato(legato_config);
    legato_bus.note_write(0, on(60));
    TEST_ASSERT_TRUE(run_cv_pass(legato_bus, legato, 1000).gate);
    legato_bus.note_write(0, on(64));
    TEST_ASSERT_TRUE(run_cv_pass(legato_bus, legato, 2000).gate);
}

static void test_pitch_bend_moves_a_held_note_by_the_range_set() {
    BusManager bus;
    NodeConfig c = cv_config();
    MidiToCv node(c);

    bus.note_write(0, on(60));
    const int16_t centre = run_cv_pass(bus, node).pitch;
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(60 - MidiToCv::DEFAULT_BASE), centre);

    // Two semitones up at full travel, by default.
    bus.note_write(0, bend_to(16383));
    TEST_ASSERT_INT16_WITHIN(1, semitones_above_base(60 - MidiToCv::DEFAULT_BASE + 2),
                             run_cv_pass(bus, node).pitch);
    // ... and two down at the bottom.
    bus.note_write(0, bend_to(0));
    TEST_ASSERT_INT16_WITHIN(1, semitones_above_base(60 - MidiToCv::DEFAULT_BASE - 2),
                             run_cv_pass(bus, node).pitch);
    // Back to the centre, exactly: a wheel at rest is not a detune.
    bus.note_write(0, bend_to(MidiToCv::BEND_CENTRE));
    TEST_ASSERT_EQUAL_INT16(centre, run_cv_pass(bus, node).pitch);

    // A wider range is the same deflection, further.
    bus.note_write(0, bend_to(16383));
    TEST_ASSERT_TRUE(node.set_param(3, 12));
    TEST_ASSERT_INT16_WITHIN(1, semitones_above_base(60 - MidiToCv::DEFAULT_BASE + 12),
                             run_cv_pass(bus, node).pitch);

    // And zero is a converter that ignores the wheel altogether.
    TEST_ASSERT_TRUE(node.set_param(3, 0));
    TEST_ASSERT_EQUAL_INT16(centre, run_cv_pass(bus, node).pitch);
}

static void test_velocity_is_taken_at_the_attack_and_held_after_it() {
    BusManager bus;
    NodeConfig c = cv_config();
    MidiToCv node(c);

    bus.note_write(0, on(60, 127));
    TEST_ASSERT_EQUAL_INT16(CV_MAX, run_cv_pass(bus, node).velocity);

    bus.note_write(0, on(64, 64));
    TEST_ASSERT_EQUAL_INT16((int32_t)64 * CV_MAX / 127, run_cv_pass(bus, node, 1000).velocity);

    // Released, and the level stays where it was: an envelope in its release
    // is still reading this.
    bus.note_write(0, off(64));
    bus.note_write(0, off(60));
    const CvVoice after = run_cv_pass(bus, node, 2000);
    TEST_ASSERT_FALSE(after.gate);
    TEST_ASSERT_EQUAL_INT16((int32_t)64 * CV_MAX / 127, after.velocity);
}

static void test_the_mod_outlet_follows_the_controller_it_is_pointed_at() {
    BusManager bus;
    NodeConfig c = cv_config();
    MidiToCv node(c);

    // The mod wheel, by default.
    bus.note_write(0, cc(1, 127));
    TEST_ASSERT_EQUAL_INT16(CV_MAX, run_cv_pass(bus, node).mod);
    bus.note_write(0, cc(1, 0));
    TEST_ASSERT_EQUAL_INT16(0, run_cv_pass(bus, node).mod);
    bus.note_write(0, cc(1, 64));
    TEST_ASSERT_EQUAL_INT16((int32_t)64 * CV_MAX / 127, run_cv_pass(bus, node).mod);

    // Another controller is ignored until it is the one asked for, and the
    // level the wheel left is kept when it changes.
    bus.note_write(0, cc(74, 127));
    TEST_ASSERT_EQUAL_INT16((int32_t)64 * CV_MAX / 127, run_cv_pass(bus, node).mod);
    TEST_ASSERT_TRUE(node.set_param(7, 74));
    TEST_ASSERT_EQUAL_INT16((int32_t)64 * CV_MAX / 127, run_cv_pass(bus, node).mod);
    bus.note_write(0, cc(74, 127));
    TEST_ASSERT_EQUAL_INT16(CV_MAX, run_cv_pass(bus, node).mod);
}

static void test_channel_pressure_can_drive_the_mod_outlet_instead() {
    BusManager bus;
    NodeConfig c = cv_config();
    c.params[6] = MidiToCv::MOD_PRESSURE;
    MidiToCv node(c);

    bus.note_write(0, cc(1, 127));                       // the wheel is not the source now
    TEST_ASSERT_EQUAL_INT16(0, run_cv_pass(bus, node).mod);

    bus.note_write(0, MidiEvent{MIDI_AFTERTOUCH_CHANNEL, 1, 100, 0});
    TEST_ASSERT_EQUAL_INT16((int32_t)100 * CV_MAX / 127, run_cv_pass(bus, node).mod);
}

static void test_an_outlet_with_nothing_patched_to_it_is_not_an_error() {
    BusManager bus;
    NodeConfig c = cv_config();
    c.out_bus[0] = NO_BUS;                               // no pitch
    c.out_bus[2] = NO_BUS;                               // no velocity
    c.out_bus[3] = NO_BUS;                               // no mod
    c.out_bus[4] = NO_BUS;                               // no trigger
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));

    MidiToCv node(c);
    bus.note_write(0, on(60));
    const CvVoice v = run_cv_pass(bus, node, 1000);
    TEST_ASSERT_TRUE(v.gate);                            // the one jack that is patched
    TEST_ASSERT_EQUAL_INT16(0, v.pitch);                 // and nothing written anywhere else
    TEST_ASSERT_EQUAL_INT16(0, v.velocity);
    TEST_ASSERT_FALSE(v.trigger);
    TEST_ASSERT_EQUAL_INT16(semitones_above_base(60 - MidiToCv::DEFAULT_BASE), node.pitch());
}

static void test_the_voice_survives_more_keys_than_the_node_can_hold() {
    BusManager bus;
    NodeConfig c = cv_config();
    c.params[0] = MidiToCv::PRIORITY_LOWEST;
    MidiToCv node(c);

    // Past MAX_HELD_NOTES the oldest key is evicted. The voice is re-derived
    // from what is held, so the eviction can move it but can never strand it.
    for (uint8_t i = 0; i < MAX_HELD_NOTES + 4; i++) bus.note_write(0, on((uint8_t)(40 + i)));
    const CvVoice full = run_cv_pass(bus, node, 1000);
    TEST_ASSERT_TRUE(full.gate);
    TEST_ASSERT_EQUAL(MAX_HELD_NOTES, node.held_count());
    TEST_ASSERT_EQUAL(44, node.voice());                 // the four oldest were evicted

    for (uint8_t i = 0; i < MAX_HELD_NOTES + 4; i++) bus.note_write(0, off((uint8_t)(40 + i)));
    const CvVoice empty = run_cv_pass(bus, node, 2000);
    TEST_ASSERT_FALSE(empty.gate);
    TEST_ASSERT_EQUAL(0, node.held_count());
    TEST_ASSERT_EQUAL(HeldNotes::NONE, node.voice());
}

// ---------------------------------------------------------------------------
// The rule that governs every modifier: no hanging notes, ever
// ---------------------------------------------------------------------------

// `unlatch` is for a node that is *meant* to keep playing with every key
// released - an arpeggiator on hold. The storm ends the same way a player
// does: the latch comes off, and then nothing may be left sounding.
template <class T>
static void assert_no_hanging_notes(NodeConfig config, uint8_t in_bus, uint8_t out_bus,
                                    uint32_t seed, bool with_gate, uint16_t unlatch = 0xFFFF) {
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
    if (unlatch != 0xFFFF) {
        TEST_ASSERT_TRUE(node.set_param(unlatch, 0));
        balance.observe(run_pass(bus, node, out_bus, now + 200000));
    }

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

static void test_note_quantise_hangs_nothing() {
    NodeConfig c = modifier_config(ALGO_NOTE_QUANTISE);
    c.params[0] = SCALE_BLUES;
    assert_no_hanging_notes<NoteQuantise>(c, 0, 3, 5, false);
}

static void test_probability_hangs_nothing() {
    NodeConfig c = modifier_config(ALGO_PROBABILITY);
    c.params[0] = 60;
    assert_no_hanging_notes<Probability>(c, 0, 3, 6, false);
}

// Hold is the one state where a modifier keeps notes with nothing held, so
// it gets the storm too: what the latch keeps must still be released when it
// comes off.
static void test_arpeggiator_on_hold_hangs_nothing() {
    for (uint8_t mode = 0; mode <= Arpeggiator::ARP_AS_PLAYED; mode++) {
        NodeConfig c = modifier_config(ALGO_ARPEGGIATOR);
        c.in_bus[1] = 1;
        c.params[0] = mode;
        c.params[1] = 2;
        c.params[4] = 1;                               // hold
        assert_no_hanging_notes<Arpeggiator>(c, 0, 3, 21u + mode, true, 4);
    }
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
    RUN_TEST(test_held_notes_answers_the_priority_rules_and_finds_a_note);
    RUN_TEST(test_release_all_emits_only_note_offs);
    RUN_TEST(test_sounding_notes_refuses_what_it_cannot_release);
    RUN_TEST(test_scale_masks_and_quantisation);
    RUN_TEST(test_transpose_releases_what_it_sent_after_the_offset_moves);
    RUN_TEST(test_transpose_drops_out_of_range_notes_and_their_offs);
    RUN_TEST(test_note_priority_modes);
    RUN_TEST(test_note_priority_falls_back_on_release);
    RUN_TEST(test_velocity_curves_never_reach_zero);
    RUN_TEST(test_velocity_curve_shapes_and_passes_offs);
    RUN_TEST(test_chord_emits_every_voice_and_releases_all_of_it);
    RUN_TEST(test_note_quantise_snaps_and_releases_what_it_sent);
    RUN_TEST(test_note_quantise_takes_its_root_from_a_bus);
    RUN_TEST(test_the_global_scale_is_the_default_and_an_override_wins);
    RUN_TEST(test_the_key_can_name_a_register);
    RUN_TEST(test_the_register_resolves_for_both_kinds_of_root);
    RUN_TEST(test_a_self_playing_chord_follows_the_key_register);
    RUN_TEST(test_note_quantise_follows_the_module_scale_until_it_names_one);
    RUN_TEST(test_chord_voices_its_quality_in_the_scale);
    RUN_TEST(test_chord_in_the_chromatic_scale_is_fixed_semitones);
    RUN_TEST(test_a_chord_with_no_note_inlet_plays_itself_and_holds);
    RUN_TEST(test_a_self_playing_chord_takes_its_octave_and_velocity);
    RUN_TEST(test_a_sequenced_root_walks_a_self_playing_chord_through_the_key);
    RUN_TEST(test_chord_quality_names_a_stack_of_scale_steps);
    RUN_TEST(test_chord_quality_takes_its_flavour_from_the_degree);
    RUN_TEST(test_chord_inversion_moves_the_bass);
    RUN_TEST(test_chord_voicing_opens_the_stack);
    RUN_TEST(test_a_repeated_root_re_strikes_only_when_asked);
    RUN_TEST(test_editing_a_self_playing_chord_re_voices_it);
    RUN_TEST(test_a_self_playing_chord_feeds_an_arpeggiator);
    RUN_TEST(test_probability_pairs_every_note_it_passes);
    RUN_TEST(test_probability_defaults_to_passing_everything);
    RUN_TEST(test_arpeggiator_one_note_per_edge_in_order);
    RUN_TEST(test_arpeggiator_up_down_does_not_repeat_the_endpoints);
    RUN_TEST(test_arpeggiator_octave_range_and_reset);
    RUN_TEST(test_arpeggiator_releases_when_the_chord_is_lifted);
    RUN_TEST(test_arpeggiator_gate_length_releases_early);
    RUN_TEST(test_arpeggiator_hold_keeps_the_figure_after_the_keys_are_lifted);
    RUN_TEST(test_arpeggiator_hold_replaces_the_chord_rather_than_adding_to_it);
    RUN_TEST(test_arpeggiator_hold_off_drops_only_what_no_key_holds);
    RUN_TEST(test_arpeggiator_hold_inlet_latches_like_the_parameter);

    RUN_TEST(test_midi_to_cv_pitch_is_an_octave_per_range_step);
    RUN_TEST(test_a_note_below_the_base_clamps_at_the_bottom);
    RUN_TEST(test_the_gate_is_up_for_as_long_as_a_key_is_held);
    RUN_TEST(test_the_priority_decides_which_held_note_the_voice_takes);
    RUN_TEST(test_releasing_a_key_hands_the_voice_back_without_a_new_attack);
    RUN_TEST(test_the_trigger_is_a_fixed_width_pulse_on_every_attack);
    RUN_TEST(test_retrigger_mode_drops_the_gate_for_one_pass);
    RUN_TEST(test_pitch_bend_moves_a_held_note_by_the_range_set);
    RUN_TEST(test_velocity_is_taken_at_the_attack_and_held_after_it);
    RUN_TEST(test_the_mod_outlet_follows_the_controller_it_is_pointed_at);
    RUN_TEST(test_channel_pressure_can_drive_the_mod_outlet_instead);
    RUN_TEST(test_an_outlet_with_nothing_patched_to_it_is_not_an_error);
    RUN_TEST(test_the_voice_survives_more_keys_than_the_node_can_hold);
    RUN_TEST(test_transpose_hangs_nothing);
    RUN_TEST(test_note_priority_hangs_nothing);
    RUN_TEST(test_velocity_curve_hangs_nothing);
    RUN_TEST(test_chord_hangs_nothing);
    RUN_TEST(test_note_quantise_hangs_nothing);
    RUN_TEST(test_probability_hangs_nothing);
    RUN_TEST(test_arpeggiator_hangs_nothing_in_any_mode);
    RUN_TEST(test_arpeggiator_on_hold_hangs_nothing);
    return UNITY_END();
}
