#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/note_event.h"
#include "algorithm/midi/note_filter.h"
#include "algorithm/midi/channel.h"

void setUp() {}
void tearDown() {}

// The two nodes that decide where a note goes rather than what it is:
// NoteFilter says whether it passes, Channel says which channel it leaves on.
// Both own the note-off for every note-on they emit, and most of what is
// asserted here is that ownership under parameters that move.

// ---------------------------------------------------------------------------
// Test rig: one pass around a node, exactly as MixedModeMaster runs it.
// ---------------------------------------------------------------------------

static std::vector<MidiEvent> run_pass(BusManager& bus, Node& node, uint8_t out_bus) {
    bus.swap();
    node.process(bus, 0);
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
static MidiEvent cc(uint8_t controller, uint8_t value, uint8_t channel = 1) {
    return MidiEvent{MIDI_CONTROL_CHANGE, channel, controller, value};
}

static NodeConfig filter_config(uint8_t in_bus, uint8_t out_bus) {
    NodeConfig c = node_config(ALGO_NOTE_FILTER);
    c.in_bus[0] = in_bus;
    c.out_bus[0] = out_bus;
    return c;
}

static NodeConfig channel_config(uint8_t in_bus, uint8_t out_bus,
                                 uint8_t first = 0, uint8_t count = 0) {
    NodeConfig c = node_config(ALGO_CHANNEL);
    c.in_bus[0] = in_bus;
    c.out_bus[0] = out_bus;
    c.params[Channel::P_CHANNEL] = first;
    c.params[Channel::P_COUNT] = count;
    return c;
}

// Every note-on must be released by the time the run ends, and no note-off
// may ever outnumber the note-ons before it. This is the "no hanging notes"
// ledger both suites assert against.
struct NoteBalance {
    int8_t sounding[16][128];
    bool went_negative;

    NoteBalance() : sounding(), went_negative(false) {}

    void observe(const std::vector<MidiEvent>& events) {
        for (const MidiEvent& e : events) {
            const uint8_t ch = (uint8_t)(e.channel - 1);
            if (is_note_on(e)) sounding[ch][e.data1]++;
            else if (is_note_off(e)) {
                sounding[ch][e.data1]--;
                if (sounding[ch][e.data1] < 0) went_negative = true;
            }
        }
    }
    uint16_t total() const {
        uint16_t n = 0;
        for (uint8_t c = 0; c < 16; c++)
            for (uint8_t i = 0; i < 128; i++)
                if (sounding[c][i] > 0) n = (uint16_t)(n + sounding[c][i]);
        return n;
    }
};

// A deterministic stream, so a failure reproduces exactly.
struct NoteScript {
    uint32_t state;
    explicit NoteScript(uint32_t seed) : state(seed) {}
    uint32_t next() { state = state * 1664525u + 1013904223u; return state; }
    uint8_t note() { return (uint8_t)(36 + (next() >> 16) % 48); }
    uint8_t velocity() { return (uint8_t)(1 + (next() >> 16) % 127); }
    uint8_t channel() { return (uint8_t)(1 + (next() >> 16) % 4); }
    bool on_event() { return ((next() >> 16) & 3) != 0; }
};

// ---------------------------------------------------------------------------
// NoteFilter: what passes
// ---------------------------------------------------------------------------

static void test_an_unconfigured_filter_passes_everything() {
    BusManager bus;
    NoteFilter node(filter_config(0, 1));
    bus.note_write(0, on(60));
    bus.note_write(0, cc(74, 40));
    bus.note_write(0, MidiEvent{MIDI_PITCH_BEND, 1, 0, 64});
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, out[0].type);
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(100, out[0].data2);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, out[1].type);
    TEST_ASSERT_EQUAL(MIDI_PITCH_BEND, out[2].type);
}

static void test_a_note_window_keeps_what_is_inside_it() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_LOW] = 60;
    c.params[NoteFilter::P_HIGH] = 72;
    NoteFilter node(c);
    bus.note_write(0, on(59));
    bus.note_write(0, on(60));
    bus.note_write(0, on(72));
    bus.note_write(0, on(73));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, out.size());       // the bounds are inclusive
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(72, out[1].data1);
}

// Two filters on one bus, rejoined on another, are a keyboard split. This is
// why the node has one outlet and not several: the bus is the router.
static void test_two_filters_on_one_bus_are_a_keyboard_split() {
    BusManager bus;
    NodeConfig lower = filter_config(0, 1);
    lower.params[NoteFilter::P_HIGH] = 59;
    NodeConfig upper = filter_config(0, 2);
    upper.params[NoteFilter::P_LOW] = 60;
    NoteFilter bass(lower);
    NoteFilter lead(upper);

    bus.note_write(0, on(48));
    bus.note_write(0, on(64));
    bus.swap();
    bass.process(bus, 0);
    lead.process(bus, 0);
    bus.swap();
    TEST_ASSERT_EQUAL(1, bus.note_count(1));
    TEST_ASSERT_EQUAL(48, bus.note_read(1, 0).data1);
    TEST_ASSERT_EQUAL(1, bus.note_count(2));
    TEST_ASSERT_EQUAL(64, bus.note_read(2, 0).data1);
}

static void test_a_velocity_window_is_a_dynamic_layer() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_VEL_MIN] = 80;
    NoteFilter node(c);
    bus.note_write(0, on(60, 79));
    bus.note_write(0, on(62, 80));
    bus.note_write(0, on(64, 127));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(62, out[0].data1);
    TEST_ASSERT_EQUAL(64, out[1].data1);
}

// A note-off carries velocity 0 and would fail any velocity window. It is
// never tested: the pass is looked up in the ledger.
static void test_a_note_off_is_looked_up_not_tested() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_VEL_MIN] = 50;
    NoteFilter node(c);
    bus.note_write(0, on(60, 100));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(1, node.sounding_count());

    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_TRUE(is_note_off(out[0]));
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

static void test_a_note_off_whose_note_on_was_dropped_is_dropped_too() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_LOW] = 60;
    NoteFilter node(c);
    bus.note_write(0, on(48));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(0, out.size());

    bus.note_write(0, off(48));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(0, out.size());      // nothing was recorded, nothing owed
}

// The failure this class of node is prone to: move the window under a held
// note and the release must still go out.
static void test_moving_the_window_cannot_strand_a_held_note() {
    BusManager bus;
    NoteFilter node(filter_config(0, 1));
    bus.note_write(0, on(60));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());

    // The window closes over the note while it is still held.
    TEST_ASSERT_TRUE(node.set_param(NoteFilter::P_LOW, 100));
    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_TRUE(is_note_off(out[0]));
    TEST_ASSERT_EQUAL(60, out[0].data1);
}

static void test_a_channel_of_zero_is_any_channel() {
    BusManager bus;
    NoteFilter node(filter_config(0, 1));
    bus.note_write(0, on(60, 100, 1));
    bus.note_write(0, on(61, 100, 9));
    bus.note_write(0, on(62, 100, 16));
    TEST_ASSERT_EQUAL(3, run_pass(bus, node, 1).size());
}

static void test_a_channel_keeps_only_that_channel() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_CHANNEL] = 10;
    NoteFilter node(c);
    bus.note_write(0, on(36, 100, 10));
    bus.note_write(0, on(60, 100, 1));
    bus.note_write(0, cc(74, 40, 10));
    bus.note_write(0, cc(74, 40, 1));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(36, out[0].data1);
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, out[1].type);
    TEST_ASSERT_EQUAL(10, out[1].channel);
}

// One node is "everything that is not the drum channel", which would
// otherwise be fifteen of them.
static void test_not_channel_keeps_every_channel_but_that_one() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_CHANNEL] = 10;
    c.params[NoteFilter::P_NOT_CHANNEL] = 1;
    NoteFilter node(c);
    bus.note_write(0, on(36, 100, 10));
    bus.note_write(0, on(60, 100, 1));
    bus.note_write(0, on(62, 100, 16));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(62, out[1].data1);
}

// It inverts the channel and nothing else, which is what leaves "the notes,
// but not the ones on channel 10" sayable in one node.
static void test_not_channel_leaves_the_other_tests_alone() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_CHANNEL] = 10;
    c.params[NoteFilter::P_NOT_CHANNEL] = 1;
    c.params[NoteFilter::P_PASS] = NoteFilter::PASS_NOTES;
    c.params[NoteFilter::P_LOW] = 60;
    NoteFilter node(c);
    bus.note_write(0, on(60, 100, 1));    // kept
    bus.note_write(0, on(48, 100, 1));    // below the window, still dropped
    bus.note_write(0, cc(74, 40, 1));     // not a note, still dropped
    bus.note_write(0, on(60, 100, 10));   // the excluded channel
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(60, out[0].data1);
    TEST_ASSERT_EQUAL(1, out[0].channel);
}

static void test_pass_notes_strips_everything_that_is_not_a_note() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_PASS] = NoteFilter::PASS_NOTES;
    NoteFilter node(c);
    bus.note_write(0, on(60));
    bus.note_write(0, cc(74, 40));
    bus.note_write(0, MidiEvent{MIDI_PITCH_BEND, 1, 0, 64});
    bus.note_write(0, MidiEvent{MIDI_AFTERTOUCH_CHANNEL, 1, 90, 0});
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, out[0].type);
}

static void test_pass_controls_strips_the_notes() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_PASS] = NoteFilter::PASS_CONTROLS;
    NoteFilter node(c);
    bus.note_write(0, on(60));
    bus.note_write(0, off(60));
    bus.note_write(0, cc(74, 40));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, out[0].type);
}

// A pitch window has nothing to say about a message with no pitch: a CC's
// controller number is not a note.
static void test_a_note_window_does_not_judge_a_controller_number() {
    BusManager bus;
    NodeConfig c = filter_config(0, 1);
    c.params[NoteFilter::P_LOW] = 60;
    c.params[NoteFilter::P_HIGH] = 72;
    NoteFilter node(c);
    bus.note_write(0, cc(1, 40));       // controller 1, well below the window
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
}

static void test_silence_releases_what_the_filter_passed() {
    BusManager bus;
    NoteFilter node(filter_config(0, 1));
    bus.note_write(0, on(60));
    bus.note_write(0, on(64));
    run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(2, node.sounding_count());

    bus.swap();
    node.silence(bus);
    bus.swap();
    TEST_ASSERT_EQUAL(2, bus.note_count(1));
    TEST_ASSERT_TRUE(is_note_off(bus.note_read(1, 0)));
    TEST_ASSERT_TRUE(is_note_off(bus.note_read(1, 1)));
    TEST_ASSERT_EQUAL(0, node.sounding_count());
}

static void test_note_filter_hangs_nothing() {
    BusManager bus;
    NoteFilter node(filter_config(0, 1));
    NoteBalance balance;
    NoteScript script(20260912u);
    for (int pass = 0; pass < 600; pass++) {
        // Every parameter moves under the stream, which is the whole point.
        node.set_param(NoteFilter::P_LOW, (uint8_t)(script.next() % 128));
        node.set_param(NoteFilter::P_HIGH, (uint8_t)(1 + script.next() % 127));
        node.set_param(NoteFilter::P_VEL_MIN, (uint8_t)(1 + script.next() % 127));
        node.set_param(NoteFilter::P_VEL_MAX, (uint8_t)(1 + script.next() % 127));
        node.set_param(NoteFilter::P_CHANNEL, (uint8_t)(script.next() % 17));
        node.set_param(NoteFilter::P_PASS, (uint8_t)(script.next() % NoteFilter::PASS_MODES));
        node.set_param(NoteFilter::P_NOT_CHANNEL, (uint8_t)(script.next() % 2));
        const uint8_t note = script.note();
        bus.note_write(0, script.on_event() ? on(note, script.velocity(), script.channel())
                                            : off(note, script.channel()));
        balance.observe(run_pass(bus, node, 1));
    }
    TEST_ASSERT_FALSE(balance.went_negative);

    bus.swap();
    node.silence(bus);
    bus.swap();
    std::vector<MidiEvent> tail;
    for (uint8_t i = 0; i < bus.note_count(1); i++) tail.push_back(bus.note_read(1, i));
    balance.observe(tail);
    TEST_ASSERT_EQUAL(0, balance.total());
}

// ---------------------------------------------------------------------------
// Channel: where it leaves on
// ---------------------------------------------------------------------------

static void test_a_span_of_one_puts_everything_on_that_channel() {
    BusManager bus;
    Channel node(channel_config(0, 1, 7, 1));
    bus.note_write(0, on(60, 100, 1));
    bus.note_write(0, on(64, 100, 2));
    bus.note_write(0, cc(74, 40, 3));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    for (const MidiEvent& e : out) TEST_ASSERT_EQUAL(7, e.channel);
}

static void test_a_span_spreads_notes_one_per_channel() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 4));
    bus.note_write(0, on(60));
    bus.note_write(0, on(64));
    bus.note_write(0, on(67));
    bus.note_write(0, on(71));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(4, out.size());
    TEST_ASSERT_EQUAL(1, out[0].channel);
    TEST_ASSERT_EQUAL(2, out[1].channel);
    TEST_ASSERT_EQUAL(3, out[2].channel);
    TEST_ASSERT_EQUAL(4, out[3].channel);
    TEST_ASSERT_EQUAL(60, out[0].data1);   // the pitch is untouched
    TEST_ASSERT_EQUAL(71, out[3].data1);
}

static void test_a_span_wraps_at_sixteen() {
    BusManager bus;
    Channel node(channel_config(0, 1, 15, 4));
    TEST_ASSERT_EQUAL(15, node.channel_at(0));
    TEST_ASSERT_EQUAL(16, node.channel_at(1));
    TEST_ASSERT_EQUAL(1, node.channel_at(2));
    TEST_ASSERT_EQUAL(2, node.channel_at(3));
    bus.note_write(0, on(60));
    bus.note_write(0, on(62));
    bus.note_write(0, on(64));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(15, out[0].channel);
    TEST_ASSERT_EQUAL(16, out[1].channel);
    TEST_ASSERT_EQUAL(1, out[2].channel);
}

// A note allocated to channel 3 and released on channel 1 never stops.
static void test_a_note_off_leaves_on_the_channel_its_note_on_did() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 4));
    bus.note_write(0, on(60));
    bus.note_write(0, on(64));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out[0].channel);
    TEST_ASSERT_EQUAL(2, out[1].channel);

    bus.note_write(0, off(64));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_TRUE(is_note_off(out[0]));
    TEST_ASSERT_EQUAL(64, out[0].data1);
    TEST_ASSERT_EQUAL(2, out[0].channel);
}

// Blind round-robin would hand a note to a busy channel while a free one sat
// idle. The freed channel is the one the next note takes.
static void test_allocation_prefers_a_channel_with_nothing_sounding() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 2));
    bus.note_write(0, on(60));
    bus.note_write(0, on(64));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out[0].channel);
    TEST_ASSERT_EQUAL(2, out[1].channel);

    bus.note_write(0, off(60));           // channel 1 is free again
    run_pass(bus, node, 1);
    bus.note_write(0, on(67));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(1, out[0].channel);
}

// With no free voice to find there is nothing to prefer, so the round robin
// decides and the span keeps being used evenly.
static void test_a_full_span_keeps_rotating() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 2));
    bus.note_write(0, on(60));
    bus.note_write(0, on(64));
    bus.note_write(0, on(67));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(1, out[0].channel);
    TEST_ASSERT_EQUAL(2, out[1].channel);
    TEST_ASSERT_EQUAL(1, out[2].channel);
}

static void test_moving_the_span_cannot_strand_a_held_note() {
    BusManager bus;
    Channel node(channel_config(0, 1, 5, 4));
    bus.note_write(0, on(60));
    std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(5, out[0].channel);

    // The span moves somewhere else entirely while the note is held.
    TEST_ASSERT_TRUE(node.set_param(Channel::P_CHANNEL, 12));
    TEST_ASSERT_TRUE(node.set_param(Channel::P_COUNT, 1));
    bus.note_write(0, off(60));
    out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(5, out[0].channel);      // as sent, not as configured
}

static void test_narrowing_the_span_keeps_the_cursor_inside_it() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 8));
    for (uint8_t i = 0; i < 6; i++) bus.note_write(0, on((uint8_t)(60 + i)));
    run_pass(bus, node, 1);                    // the cursor is now at slot 6
    for (uint8_t i = 0; i < 6; i++) bus.note_write(0, off((uint8_t)(60 + i)));
    run_pass(bus, node, 1);

    TEST_ASSERT_TRUE(node.set_param(Channel::P_COUNT, 2));
    bus.note_write(0, on(72));
    const std::vector<MidiEvent> out = run_pass(bus, node, 1);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_TRUE(out[0].channel == 1 || out[0].channel == 2);
}

static void test_a_span_refuses_a_channel_outside_midi() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 1));
    TEST_ASSERT_FALSE(node.set_param(Channel::P_CHANNEL, 0));
    TEST_ASSERT_FALSE(node.set_param(Channel::P_CHANNEL, 17));
    TEST_ASSERT_FALSE(node.set_param(Channel::P_COUNT, 0));
    TEST_ASSERT_FALSE(node.set_param(Channel::P_COUNT, 17));
    TEST_ASSERT_EQUAL(1, node.get_param(Channel::P_CHANNEL));
    TEST_ASSERT_EQUAL(1, node.get_param(Channel::P_COUNT));
    (void)bus;
}

static void test_channel_hangs_nothing() {
    BusManager bus;
    Channel node(channel_config(0, 1, 1, 4));
    NoteBalance balance;
    NoteScript script(4071981u);
    for (int pass = 0; pass < 600; pass++) {
        node.set_param(Channel::P_CHANNEL, (uint8_t)(1 + script.next() % 16));
        node.set_param(Channel::P_COUNT, (uint8_t)(1 + script.next() % 16));
        const uint8_t note = script.note();
        bus.note_write(0, script.on_event() ? on(note, script.velocity(), script.channel())
                                            : off(note, script.channel()));
        balance.observe(run_pass(bus, node, 1));
    }
    TEST_ASSERT_FALSE(balance.went_negative);

    bus.swap();
    node.silence(bus);
    bus.swap();
    std::vector<MidiEvent> tail;
    for (uint8_t i = 0; i < bus.note_count(1); i++) tail.push_back(bus.note_read(1, i));
    balance.observe(tail);
    TEST_ASSERT_EQUAL(0, balance.total());
}

// ---------------------------------------------------------------------------
// The two together: the patch they exist for.
// ---------------------------------------------------------------------------

// Take the drum channel off a merged stream and give what is left four mono
// synths. Neither node can do this alone.
static void test_a_filter_into_a_span_is_a_multitimbral_split() {
    BusManager bus;
    NodeConfig f = filter_config(0, 1);
    f.params[NoteFilter::P_CHANNEL] = 10;
    f.params[NoteFilter::P_NOT_CHANNEL] = 1;
    f.params[NoteFilter::P_PASS] = NoteFilter::PASS_NOTES;
    NoteFilter keys(f);
    Channel spread(channel_config(1, 2, 1, 4));

    bus.note_write(0, on(36, 100, 10));       // a kick, on the drum channel
    bus.note_write(0, on(60, 100, 1));
    bus.note_write(0, on(64, 100, 2));
    bus.note_write(0, cc(74, 40, 1));         // and a filter sweep

    bus.swap();
    keys.process(bus, 0);
    // Bus 1's last writer has run, so the schedule publishes it here and the
    // reader sees it in the same pass (node/schedule.h).
    bus.publish(0, (uint16_t)(1u << 1), 0);
    spread.process(bus, 0);
    bus.swap();

    TEST_ASSERT_EQUAL(2, bus.note_count(2));
    TEST_ASSERT_EQUAL(60, bus.note_read(2, 0).data1);
    TEST_ASSERT_EQUAL(1, bus.note_read(2, 0).channel);
    TEST_ASSERT_EQUAL(64, bus.note_read(2, 1).data1);
    TEST_ASSERT_EQUAL(2, bus.note_read(2, 1).channel);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_an_unconfigured_filter_passes_everything);
    RUN_TEST(test_a_note_window_keeps_what_is_inside_it);
    RUN_TEST(test_two_filters_on_one_bus_are_a_keyboard_split);
    RUN_TEST(test_a_velocity_window_is_a_dynamic_layer);
    RUN_TEST(test_a_note_off_is_looked_up_not_tested);
    RUN_TEST(test_a_note_off_whose_note_on_was_dropped_is_dropped_too);
    RUN_TEST(test_moving_the_window_cannot_strand_a_held_note);
    RUN_TEST(test_a_channel_of_zero_is_any_channel);
    RUN_TEST(test_a_channel_keeps_only_that_channel);
    RUN_TEST(test_not_channel_keeps_every_channel_but_that_one);
    RUN_TEST(test_not_channel_leaves_the_other_tests_alone);
    RUN_TEST(test_pass_notes_strips_everything_that_is_not_a_note);
    RUN_TEST(test_pass_controls_strips_the_notes);
    RUN_TEST(test_a_note_window_does_not_judge_a_controller_number);
    RUN_TEST(test_silence_releases_what_the_filter_passed);
    RUN_TEST(test_note_filter_hangs_nothing);

    RUN_TEST(test_a_span_of_one_puts_everything_on_that_channel);
    RUN_TEST(test_a_span_spreads_notes_one_per_channel);
    RUN_TEST(test_a_span_wraps_at_sixteen);
    RUN_TEST(test_a_note_off_leaves_on_the_channel_its_note_on_did);
    RUN_TEST(test_allocation_prefers_a_channel_with_nothing_sounding);
    RUN_TEST(test_a_full_span_keeps_rotating);
    RUN_TEST(test_moving_the_span_cannot_strand_a_held_note);
    RUN_TEST(test_narrowing_the_span_keeps_the_cursor_inside_it);
    RUN_TEST(test_a_span_refuses_a_channel_outside_midi);
    RUN_TEST(test_channel_hangs_nothing);

    RUN_TEST(test_a_filter_into_a_span_is_a_multitimbral_split);
    return UNITY_END();
}
