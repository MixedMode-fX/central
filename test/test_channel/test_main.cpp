#include <unity.h>
#include <stdio.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/node_pool.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/note_event.h"
#include "midi/global_key.h"
#include "algorithm/midi/probability.h"
#include "algorithm/midi/velocity_curve.h"

void setUp() {}
void tearDown() { global_key::set(SCALE_CHROMATIC, 0); }

// The channel every node sends on (src/midi/note_event.h).
//
// A generator names one outright and a modifier inherits the one its stream
// arrived on, so a modifier's control is a PARAM_CHANNEL_OUT byte whose zero
// means "leave it alone". Three claims, asserted against every modifier that
// has one rather than against a list written out here, so a node added later
// is held to them without this file being edited:
//
//   * left alone, everything it emits keeps the channel it came in on;
//   * set, everything it emits leaves on the channel named - the notes it
//     plays, and the CCs and bends it is only passing through;
//   * moved while a note is sounding, nothing hangs: the note-off is sent
//     where the note-on went, which is the one thing a channel control can
//     get wrong that a user cannot recover from.

static const uint8_t B_NOTES = 0, B_OUT = 3, B_DROPPED = 4, B_GATE = 1;

// The source channel everything below plays on: not 1, so a node that quietly
// substituted a default would be caught.
static const uint8_t SOURCE_CHANNEL = 7;
static const uint8_t WANTED_CHANNEL = 5;

// What a modifier needs to be made to play. Only two of them need an edge -
// an arpeggiator steps and a retrigger strikes - and the flag says which
// inlet carries it rather than leaving the driver to guess from the domains:
// NoteDelay's gate inlet is `clear`, and pulsing that would empty the node
// under the test rather than drive it.
struct Driven {
    uint8_t id;
    bool    clocked;        // inlet 1 is the edge that makes it emit
};

static const Driven DRIVEN[] = {
    {ALGO_TRANSPOSE,      false},
    {ALGO_NOTE_QUANTISE,  false},
    {ALGO_MIRROR,         false},
    {ALGO_CHORD,          false},
    {ALGO_NOTE_PRIORITY,  false},
    {ALGO_VELOCITY,       false},
    {ALGO_VOICER,         false},
    {ALGO_PROBABILITY,    false},
    {ALGO_ARPEGGIATOR,    true},
    {ALGO_RETRIGGER,      true},
};
static const uint8_t N_DRIVEN = (uint8_t)(sizeof(DRIVEN) / sizeof(DRIVEN[0]));

// ---------------------------------------------------------------------------
// The contract, read off the registry
// ---------------------------------------------------------------------------

// The index of an algorithm's output-channel parameter, or NOT_FOUND.
static const uint16_t NOT_FOUND = 0xFFFF;

// `tables` says whether a repeating group counts. A drum machine names a
// channel per lane rather than per node, which is a channel select and is not
// an override, so the two questions want different answers.
static uint16_t channel_param(const AlgorithmDescriptor& d, uint8_t kind, bool tables = false) {
    for (uint8_t g = 0; g < d.n_param_groups; g++) {
        const ParamGroup& group = d.param_groups[g];
        if (group.repeat != 1 && !tables) continue;
        for (uint16_t f = 0; f < group.n_fields; f++) {
            if (group.fields[f].kind == kind) return (uint16_t)(group.first + f);
        }
    }
    return NOT_FOUND;
}

static bool emits_notes(const AlgorithmDescriptor& d) {
    for (uint8_t i = 0; i < d.n_out; i++) if (d.out_domain[i] == Domain::Note) return true;
    return false;
}

// Every node that puts a message on a note bus says which channel it goes out
// on. A generator names one outright (PARAM_CHANNEL: one of 1..16 is always
// chosen, per lane on a drum machine); a modifier has a stream to inherit
// from and names an override instead.
static void test_every_note_outlet_has_a_channel_control() {
    uint8_t checked = 0;
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        TEST_ASSERT_NOT_NULL(d);
        if (!emits_notes(*d)) continue;
        checked++;
        char msg[96];
        snprintf(msg, sizeof msg, "%s emits notes and names no channel", d->name);
        const bool says = channel_param(*d, PARAM_CHANNEL, true) != NOT_FOUND
                       || channel_param(*d, PARAM_CHANNEL_OUT, true) != NOT_FOUND;
        TEST_ASSERT_TRUE_MESSAGE(says, msg);
    }
    TEST_ASSERT_TRUE(checked >= N_DRIVEN);
}

// One control, spelled the same way everywhere: a byte from 0 to 16 whose
// default is 0. An editor draws it from the descriptor alone, so a node that
// numbered its own from 1 would put "omni" where the zero should be, and one
// that defaulted to a channel would move a stream nobody asked it to move.
static void test_the_override_is_the_same_control_everywhere() {
    uint8_t found = 0;
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        const uint16_t at = channel_param(*d, PARAM_CHANNEL_OUT);
        if (at == NOT_FOUND) continue;
        found++;
        const ParamDescriptor* pd = registry::param(*d, at);
        TEST_ASSERT_NOT_NULL(pd);
        char msg[96];
        snprintf(msg, sizeof msg, "%s: the channel override", d->name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("channel", pd->name, msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, pd->min, msg);
        TEST_ASSERT_EQUAL_MESSAGE(16, pd->max, msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, pd->def, msg);
    }
    TEST_ASSERT_TRUE(found >= N_DRIVEN);
}

// Nothing gains an override without being driven against the three claims
// above. NoteDelay is the exception, and only because it is driven at length
// by test_note_delay, where a delay line has a clock to be driven with.
static void test_every_override_is_exercised_here() {
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        if (channel_param(*d, PARAM_CHANNEL_OUT) == NOT_FOUND) continue;
        if (d->id == ALGO_NOTE_DELAY) continue;
        bool driven = false;
        for (uint8_t j = 0; j < N_DRIVEN; j++) if (DRIVEN[j].id == d->id) driven = true;
        char msg[96];
        snprintf(msg, sizeof msg, "%s has a channel override and is not driven here", d->name);
        TEST_ASSERT_TRUE_MESSAGE(driven, msg);
    }
}

// ---------------------------------------------------------------------------
// Driving one
// ---------------------------------------------------------------------------

// A node in a pool, wired note-in to note-out, with the edge inlet connected
// when it needs one.
struct Rig {
    BusManager bus;
    NodePool pool;
    Node* node;
    bool clocked;
    uint32_t now;

    // The pool owns the node and cannot be copied, and neither can this.
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Rig(const Driven& what, uint8_t override_channel, BusSet dropped_bus = BusSet{})
        : bus(), pool(), node(nullptr), clocked(what.clocked), now(0) {
        const AlgorithmDescriptor* d = registry::find(what.id);
        NodeConfig c = node_config(what.id);
        c.in_buses[0] = one_bus(B_NOTES);
        if (what.clocked) c.in_buses[1] = one_bus(B_GATE);
        c.out_buses[0] = one_bus(B_OUT);
        if (dropped_bus.any() && d->n_out > 1) c.out_buses[1] = dropped_bus;
        c.params[channel_param(*d, PARAM_CHANNEL_OUT)] = override_channel;
        node = pool.load(c);
    }

    // One master pass, collecting what left by `bus_index`. The edge is high
    // on every other pass, so a node that wants one gets one and a node that
    // wants a level sees it settle.
    std::vector<MidiEvent> pass(uint8_t bus_index = B_OUT) {
        if (clocked) bus.gate_write(B_GATE, (now / 5000u) % 2u == 1u);
        bus.swap();
        node->process(bus, now);
        bus.swap();
        now += 5000;
        return read(bus_index);
    }

    // A second outlet of the pass just run. It has to be read without running
    // another, because the next swap takes the pass's output away.
    std::vector<MidiEvent> read(uint8_t bus_index) {
        std::vector<MidiEvent> out;
        const uint8_t n = bus.note_count(bus_index);
        for (uint8_t i = 0; i < n; i++) out.push_back(bus.note_read(bus_index, i));
        return out;
    }

    void play(const MidiEvent& e) { bus.note_write(B_NOTES, e); }
    bool set_channel(uint8_t id, uint8_t value) {
        return node->set_param(channel_param(*registry::find(id), PARAM_CHANNEL_OUT), value);
    }
};

static MidiEvent on(uint8_t note, uint8_t channel) {
    return MidiEvent{MIDI_NOTE_ON, channel, note, 100};
}
static MidiEvent off(uint8_t note, uint8_t channel) {
    return MidiEvent{MIDI_NOTE_OFF, channel, note, 0};
}

// Every note-on must be released on the channel it was sent on. One counter
// per channel and pitch, because "released somewhere" is exactly the bug.
struct Balance {
    int8_t sounding[17][128];
    bool negative;

    Balance() : sounding(), negative(false) {}

    void observe(const std::vector<MidiEvent>& events) {
        for (const MidiEvent& e : events) {
            if (!is_note(e)) continue;
            const uint8_t ch = (uint8_t)(e.channel & 0x1F);
            if (ch > 16) continue;
            if (is_note_on(e)) sounding[ch][e.data1]++;
            else if (--sounding[ch][e.data1] < 0) negative = true;
        }
    }

    int total() const {
        int n = 0;
        for (uint8_t ch = 0; ch <= 16; ch++)
            for (uint16_t note = 0; note < 128; note++) n += sounding[ch][note];
        return n;
    }
};

// Holds a three-note chord down for a few passes, lifts it, and lets the node
// settle. Returns everything that left the output bus.
static std::vector<MidiEvent> play_a_chord(Rig& rig, uint8_t channel,
                                           uint16_t move_at = 0xFFFF, uint8_t move_to = 0,
                                           uint8_t id = 0) {
    static const uint8_t CHORD[3] = {60, 64, 67};
    std::vector<MidiEvent> seen;
    const auto collect = [&seen](const std::vector<MidiEvent>& v) {
        for (const MidiEvent& e : v) seen.push_back(e);
    };

    for (uint8_t i = 0; i < 3; i++) rig.play(on(CHORD[i], channel));
    for (uint16_t p = 0; p < 6; p++) {
        if (p == move_at) TEST_ASSERT_TRUE(rig.set_channel(id, move_to));
        collect(rig.pass());
    }
    for (uint8_t i = 0; i < 3; i++) rig.play(off(CHORD[i], channel));
    for (uint16_t p = 0; p < 6; p++) collect(rig.pass());
    return seen;
}

// ---------------------------------------------------------------------------
// What the override does
// ---------------------------------------------------------------------------

static void test_left_alone_a_modifier_keeps_the_incoming_channel() {
    for (uint8_t i = 0; i < N_DRIVEN; i++) {
        Rig rig(DRIVEN[i], 0);
        const std::vector<MidiEvent> seen = play_a_chord(rig, SOURCE_CHANNEL);
        const char* name = registry::find(DRIVEN[i].id)->name;
        char msg[96];
        snprintf(msg, sizeof msg, "%s moved a stream nobody asked it to move", name);
        TEST_ASSERT_TRUE_MESSAGE(seen.size() > 0, name);
        for (const MidiEvent& e : seen) TEST_ASSERT_EQUAL_UINT8_MESSAGE(SOURCE_CHANNEL, e.channel, msg);
    }
}

static void test_set_a_modifier_sends_on_the_channel_named() {
    for (uint8_t i = 0; i < N_DRIVEN; i++) {
        Rig rig(DRIVEN[i], WANTED_CHANNEL);
        const std::vector<MidiEvent> seen = play_a_chord(rig, SOURCE_CHANNEL);
        const char* name = registry::find(DRIVEN[i].id)->name;
        char msg[96];
        snprintf(msg, sizeof msg, "%s ignored its channel", name);
        TEST_ASSERT_TRUE_MESSAGE(seen.size() > 0, name);
        for (const MidiEvent& e : seen) TEST_ASSERT_EQUAL_UINT8_MESSAGE(WANTED_CHANNEL, e.channel, msg);
    }
}

// The failure this whole mechanism is about: move the control under a held
// chord and every note already in the air must still be taken down where it
// was put.
static void test_a_move_under_a_held_chord_hangs_nothing() {
    for (uint8_t i = 0; i < N_DRIVEN; i++) {
        Rig rig(DRIVEN[i], 0);
        Balance balance;
        balance.observe(play_a_chord(rig, SOURCE_CHANNEL, 2, WANTED_CHANNEL, DRIVEN[i].id));
        const char* name = registry::find(DRIVEN[i].id)->name;
        char msg[96];
        snprintf(msg, sizeof msg, "%s released a note it never sent", name);
        TEST_ASSERT_FALSE_MESSAGE(balance.negative, msg);
        snprintf(msg, sizeof msg, "%s left a note sounding", name);
        TEST_ASSERT_EQUAL_MESSAGE(0, balance.total(), msg);
    }
}

// And the same move in the other direction: from a channel back to the
// stream's own.
static void test_a_move_back_to_the_source_hangs_nothing() {
    for (uint8_t i = 0; i < N_DRIVEN; i++) {
        Rig rig(DRIVEN[i], WANTED_CHANNEL);
        Balance balance;
        balance.observe(play_a_chord(rig, SOURCE_CHANNEL, 2, 0, DRIVEN[i].id));
        const char* name = registry::find(DRIVEN[i].id)->name;
        TEST_ASSERT_FALSE_MESSAGE(balance.negative, name);
        TEST_ASSERT_EQUAL_MESSAGE(0, balance.total(), name);
    }
}

// A CC is not a note and has nothing to release, but it is still this node's
// output and moves with it.
static void test_a_message_that_is_not_a_note_moves_too() {
    for (uint8_t i = 0; i < N_DRIVEN; i++) {
        // An arpeggiator plays a figure rather than a stream and drops
        // everything that is not a note; it says so in its own header.
        if (DRIVEN[i].id == ALGO_ARPEGGIATOR) continue;
        Rig rig(DRIVEN[i], WANTED_CHANNEL);
        rig.play(MidiEvent{MIDI_CONTROL_CHANGE, SOURCE_CHANNEL, 74, 40});
        const std::vector<MidiEvent> seen = rig.pass();
        const char* name = registry::find(DRIVEN[i].id)->name;
        TEST_ASSERT_EQUAL_MESSAGE(1, seen.size(), name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(MIDI_CONTROL_CHANGE, seen[0].type, name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(WANTED_CHANNEL, seen[0].channel, name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(74, seen[0].data1, name);
    }
}

// A runtime write is range-checked like every other parameter: 16 channels
// and the zero that means none of them.
static void test_the_override_refuses_a_channel_that_is_not_one() {
    for (uint8_t i = 0; i < N_DRIVEN; i++) {
        Rig rig(DRIVEN[i], 0);
        const char* name = registry::find(DRIVEN[i].id)->name;
        TEST_ASSERT_TRUE_MESSAGE(rig.set_channel(DRIVEN[i].id, 16), name);
        TEST_ASSERT_FALSE_MESSAGE(rig.set_channel(DRIVEN[i].id, 17), name);
        TEST_ASSERT_FALSE_MESSAGE(rig.set_channel(DRIVEN[i].id, 255), name);
        const uint16_t at = channel_param(*registry::find(DRIVEN[i].id), PARAM_CHANNEL_OUT);
        TEST_ASSERT_EQUAL_MESSAGE(16, rig.node->get_param(at), name);
    }
}

// ---------------------------------------------------------------------------
// The two nodes with something of their own to say
// ---------------------------------------------------------------------------

// `dropped` is the other half of one split, so it moves with the notes that
// passed: a ghost part is a part, and it plays where this node plays.
static void test_probability_moves_both_of_its_outlets() {
    const Driven what = {ALGO_PROBABILITY, false};
    Rig rig(what, WANTED_CHANNEL, one_bus(B_DROPPED));
    TEST_ASSERT_TRUE(rig.node->set_param(0, 1));     // 1% chance: nearly all refused
    rig.play(on(60, SOURCE_CHANNEL));
    rig.play(on(64, SOURCE_CHANNEL));
    const std::vector<MidiEvent> passed = rig.pass(B_OUT);
    const std::vector<MidiEvent> dropped = rig.read(B_DROPPED);
    // Whichever way the dice fell, both notes left by one outlet or the
    // other: `dropped` is the other half of the split, not a report of it.
    TEST_ASSERT_EQUAL(2, passed.size() + dropped.size());
    for (const MidiEvent& e : passed) TEST_ASSERT_EQUAL_UINT8(WANTED_CHANNEL, e.channel);
    for (const MidiEvent& e : dropped) TEST_ASSERT_EQUAL_UINT8(WANTED_CHANNEL, e.channel);
}

// VelocityCurve is the one modifier with no SoundingNotes ledger - it never
// drops a note and a ledger with a capacity would make it start - so it
// remembers the channel per pitch instead. The claim is the same claim: the
// note-off follows the note-on.
static void test_velocity_curve_releases_where_it_sent() {
    const Driven what = {ALGO_VELOCITY, false};
    Rig rig(what, 0);
    rig.play(on(60, SOURCE_CHANNEL));
    std::vector<MidiEvent> seen = rig.pass();
    TEST_ASSERT_EQUAL(1, seen.size());
    TEST_ASSERT_EQUAL_UINT8(SOURCE_CHANNEL, seen[0].channel);

    TEST_ASSERT_TRUE(rig.set_channel(ALGO_VELOCITY, WANTED_CHANNEL));
    rig.play(off(60, SOURCE_CHANNEL));
    seen = rig.pass();
    TEST_ASSERT_EQUAL(1, seen.size());
    TEST_ASSERT_TRUE(is_note_off(seen[0]));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SOURCE_CHANNEL, seen[0].channel,
                                    "the note-off left where this node never sent the note-on");

    // And the next note-on is on the new channel, released there in its turn.
    rig.play(on(62, SOURCE_CHANNEL));
    seen = rig.pass();
    TEST_ASSERT_EQUAL(1, seen.size());
    TEST_ASSERT_EQUAL_UINT8(WANTED_CHANNEL, seen[0].channel);
    rig.play(off(62, SOURCE_CHANNEL));
    seen = rig.pass();
    TEST_ASSERT_EQUAL(1, seen.size());
    TEST_ASSERT_EQUAL_UINT8(WANTED_CHANNEL, seen[0].channel);
}

// The table is indexed by pitch, so it holds a note per pitch and no more -
// which is the module's held-note model everywhere else too.
static void test_velocity_curve_holds_one_channel_per_pitch() {
    const Driven what = {ALGO_VELOCITY, false};
    Rig rig(what, 0);
    Balance balance;
    for (uint8_t note = 48; note < 48 + 24; note++) rig.play(on(note, SOURCE_CHANNEL));
    balance.observe(rig.pass());
    TEST_ASSERT_TRUE(rig.set_channel(ALGO_VELOCITY, WANTED_CHANNEL));
    for (uint8_t note = 48; note < 48 + 24; note++) rig.play(off(note, SOURCE_CHANNEL));
    balance.observe(rig.pass());
    TEST_ASSERT_FALSE(balance.negative);
    TEST_ASSERT_EQUAL(0, balance.total());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_every_note_outlet_has_a_channel_control);
    RUN_TEST(test_the_override_is_the_same_control_everywhere);
    RUN_TEST(test_every_override_is_exercised_here);
    RUN_TEST(test_left_alone_a_modifier_keeps_the_incoming_channel);
    RUN_TEST(test_set_a_modifier_sends_on_the_channel_named);
    RUN_TEST(test_a_move_under_a_held_chord_hangs_nothing);
    RUN_TEST(test_a_move_back_to_the_source_hangs_nothing);
    RUN_TEST(test_a_message_that_is_not_a_note_moves_too);
    RUN_TEST(test_the_override_refuses_a_channel_that_is_not_one);
    RUN_TEST(test_probability_moves_both_of_its_outlets);
    RUN_TEST(test_velocity_curve_releases_where_it_sent);
    RUN_TEST(test_velocity_curve_holds_one_channel_per_pitch);
    return UNITY_END();
}
