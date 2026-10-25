#include <unity.h>
#include <vector>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/note_event.h"
#include "algorithm/midi/note_buffer.h"

void setUp() {}
void tearDown() {}

// NoteBuffer: every note that comes in stays on until the reset clears it.

static const uint8_t NOTE_IN = 0, NOTE_OUT = 1, GATE_RESET = 0;

static NodeConfig buffer_config(uint8_t channel = 0, bool with_reset = true) {
    NodeConfig c = node_config(ALGO_NOTE_BUFFER);
    c.in_buses[0] = one_bus(NOTE_IN);
    if (with_reset) c.in_buses[1] = one_bus(GATE_RESET);
    c.out_buses[0] = one_bus(NOTE_OUT);
    c.params[NoteBuffer::P_CHANNEL] = channel;
    return c;
}

// One pass, exactly as MixedModeMaster runs it: the inlets are published,
// then process(), then the outlet is read.
static std::vector<MidiEvent> pass(BusManager& bus, NoteBuffer& node,
                                   std::vector<MidiEvent> feed = {}, bool reset = false) {
    for (const MidiEvent& e : feed) bus.note_write(NOTE_IN, e);
    bus.gate_write(GATE_RESET, reset);
    bus.swap();
    node.process(bus, 0);
    bus.swap();
    std::vector<MidiEvent> out;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++) out.push_back(bus.note_read(NOTE_OUT, i));
    return out;
}

static MidiEvent on(uint8_t note, uint8_t velocity = 100, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_ON, channel, note, velocity};
}
static MidiEvent off(uint8_t note, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_OFF, channel, note, 0};
}

static uint8_t count(const std::vector<MidiEvent>& events, bool note_on) {
    uint8_t n = 0;
    for (const MidiEvent& e : events) if (note_on ? is_note_on(e) : is_note_off(e)) n++;
    return n;
}

static void test_the_descriptor_is_registered() {
    const AlgorithmDescriptor* d = registry::find(ALGO_NOTE_BUFFER);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("NoteBuffer", d->name);
    TEST_ASSERT_EQUAL_UINT8(2, d->n_in);
    TEST_ASSERT_EQUAL_UINT8(1, d->min_in);
    TEST_ASSERT_EQUAL(Domain::Gate, d->in_domain[1]);
    TEST_ASSERT_TRUE(sizeof(NoteBuffer) <= NODE_SLOT_SIZE);
}

// The point of the node: a note let go keeps sounding, and the next one joins it.
static void test_a_note_is_sent_at_once_and_its_note_off_is_swallowed() {
    BusManager bus;
    NodeConfig c = buffer_config();
    NoteBuffer node(c);

    std::vector<MidiEvent> out = pass(bus, node, {on(60, 90)});
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    TEST_ASSERT_TRUE(is_note_on(out[0]));
    TEST_ASSERT_EQUAL_UINT8(60, out[0].data1);
    TEST_ASSERT_EQUAL_UINT8(90, out[0].data2);

    out = pass(bus, node, {off(60)});
    TEST_ASSERT_EQUAL_UINT32(0, out.size());

    out = pass(bus, node, {on(64), off(64), on(67)});
    TEST_ASSERT_EQUAL_UINT8(2, count(out, true));
    TEST_ASSERT_EQUAL_UINT8(0, count(out, false));
    TEST_ASSERT_EQUAL_UINT8(3, node.held_count());
}

static void test_a_note_already_held_is_not_struck_again() {
    BusManager bus;
    NodeConfig c = buffer_config();
    NoteBuffer node(c);

    pass(bus, node, {on(60)});
    pass(bus, node, {off(60)});
    std::vector<MidiEvent> out = pass(bus, node, {on(60)});
    TEST_ASSERT_EQUAL_UINT32(0, out.size());
    TEST_ASSERT_EQUAL_UINT8(1, node.held_count());
}

static void test_a_reset_edge_releases_everything_once() {
    BusManager bus;
    NodeConfig c = buffer_config();
    NoteBuffer node(c);

    pass(bus, node, {on(60), on(64), on(67)});
    std::vector<MidiEvent> out = pass(bus, node, {}, true);
    TEST_ASSERT_EQUAL_UINT8(3, count(out, false));
    TEST_ASSERT_EQUAL_UINT8(0, node.held_count());

    // A reset held high is one edge, not a gate: notes played under it are held.
    out = pass(bus, node, {on(62)}, true);
    TEST_ASSERT_EQUAL_UINT8(1, count(out, true));
    TEST_ASSERT_EQUAL_UINT8(0, count(out, false));
    TEST_ASSERT_EQUAL_UINT8(1, node.held_count());
}

// The old chord goes and the new one starts, in that order on the wire.
static void test_a_note_arriving_with_the_reset_survives_it() {
    BusManager bus;
    NodeConfig c = buffer_config();
    NoteBuffer node(c);

    pass(bus, node, {on(60)});
    std::vector<MidiEvent> out = pass(bus, node, {on(60), on(65)}, true);
    TEST_ASSERT_EQUAL_UINT32(3, out.size());
    TEST_ASSERT_TRUE(is_note_off(out[0]));
    TEST_ASSERT_EQUAL_UINT8(60, out[0].data1);
    TEST_ASSERT_TRUE(is_note_on(out[1]));
    TEST_ASSERT_TRUE(is_note_on(out[2]));
    TEST_ASSERT_EQUAL_UINT8(2, node.held_count());
}

static void test_without_a_reset_only_silence_releases() {
    BusManager bus;
    NodeConfig c = buffer_config(0, false);
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
    NoteBuffer node(c);

    pass(bus, node, {on(48), on(55)});
    node.transport_stopped(bus);
    TEST_ASSERT_EQUAL_UINT8(2, node.held_count());

    node.silence(bus);
    bus.swap();
    TEST_ASSERT_EQUAL_UINT8(2, bus.note_count(NOTE_OUT));
    TEST_ASSERT_EQUAL_UINT8(0, node.held_count());
}

// A note-off goes out on the channel its note-on did, whatever the
// parameter says by then.
static void test_the_channel_override_strands_nothing() {
    BusManager bus;
    NodeConfig c = buffer_config(5);
    NoteBuffer node(c);

    std::vector<MidiEvent> out = pass(bus, node, {on(60, 100, 2)});
    TEST_ASSERT_EQUAL_UINT8(5, out[0].channel);
    TEST_ASSERT_TRUE(node.set_param(NoteBuffer::P_CHANNEL, 0));
    out = pass(bus, node, {on(62, 100, 2)});
    TEST_ASSERT_EQUAL_UINT8(2, out[0].channel);

    out = pass(bus, node, {}, true);
    TEST_ASSERT_EQUAL_UINT32(2, out.size());
    TEST_ASSERT_EQUAL_UINT8(5, out[0].channel);
    TEST_ASSERT_EQUAL_UINT8(2, out[1].channel);
}

static void test_what_is_not_a_note_passes_through() {
    BusManager bus;
    NodeConfig c = buffer_config(3);
    NoteBuffer node(c);

    std::vector<MidiEvent> out = pass(bus, node, {MidiEvent{MIDI_CONTROL_CHANGE, 1, 1, 64}});
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    TEST_ASSERT_EQUAL_UINT8(MIDI_CONTROL_CHANGE, out[0].type);
    TEST_ASSERT_EQUAL_UINT8(3, out[0].channel);
}

// Nothing is sounded that cannot be released.
static void test_a_full_buffer_drops_the_new_note() {
    BusManager bus;
    NodeConfig c = buffer_config();
    NoteBuffer node(c);

    for (uint8_t i = 0; i < MAX_SOUNDING_NOTES; i++) pass(bus, node, {on((uint8_t)(20 + i))});
    std::vector<MidiEvent> out = pass(bus, node, {on(100)});
    TEST_ASSERT_EQUAL_UINT32(0, out.size());
    TEST_ASSERT_EQUAL_UINT32(1, node.dropped());

    out = pass(bus, node, {}, true);
    TEST_ASSERT_EQUAL_UINT8(MAX_SOUNDING_NOTES, count(out, false));
}

static NodeConfig sustain_config() {
    NodeConfig c = buffer_config();
    c.params[NoteBuffer::P_MODE] = NoteBuffer::MODE_SUSTAIN;
    return c;
}

// Pedal up, the buffer is a through: the note-off follows its note-on.
static void test_sustain_with_the_pedal_up_is_a_through() {
    BusManager bus;
    NodeConfig c = sustain_config();
    NoteBuffer node(c);

    std::vector<MidiEvent> out = pass(bus, node, {on(60)});
    TEST_ASSERT_EQUAL_UINT8(1, count(out, true));
    out = pass(bus, node, {off(60)});
    TEST_ASSERT_EQUAL_UINT8(1, count(out, false));
    TEST_ASSERT_EQUAL_UINT8(0, node.held_count());

    out = pass(bus, node, {on(62), off(62)});
    TEST_ASSERT_EQUAL_UINT8(1, count(out, true));
    TEST_ASSERT_EQUAL_UINT8(1, count(out, false));
}

static void test_sustain_without_a_gate_is_a_through() {
    BusManager bus;
    NodeConfig c = buffer_config(0, false);
    c.params[NoteBuffer::P_MODE] = NoteBuffer::MODE_SUSTAIN;
    NoteBuffer node(c);

    pass(bus, node, {on(60)});
    std::vector<MidiEvent> out = pass(bus, node, {off(60)});
    TEST_ASSERT_EQUAL_UINT8(1, count(out, false));
}

// Pedal down holds what is let go; pedal up releases it, but not a key
// still down - that one waits for its own note-off.
static void test_sustain_holds_while_the_pedal_is_down() {
    BusManager bus;
    NodeConfig c = sustain_config();
    NoteBuffer node(c);

    pass(bus, node, {on(60)}, true);
    pass(bus, node, {on(64)}, true);
    std::vector<MidiEvent> out = pass(bus, node, {off(60), off(64), on(67)}, true);
    TEST_ASSERT_EQUAL_UINT8(1, count(out, true));
    TEST_ASSERT_EQUAL_UINT8(0, count(out, false));
    TEST_ASSERT_EQUAL_UINT8(3, node.held_count());

    out = pass(bus, node, {}, false);
    TEST_ASSERT_EQUAL_UINT8(2, count(out, false));
    TEST_ASSERT_EQUAL_UINT8(1, node.held_count());

    out = pass(bus, node, {off(67)});
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    TEST_ASSERT_TRUE(is_note_off(out[0]));
    TEST_ASSERT_EQUAL_UINT8(67, out[0].data1);
}

// A key held through the pedal going down is caught by it.
static void test_sustain_catches_a_key_already_down() {
    BusManager bus;
    NodeConfig c = sustain_config();
    NoteBuffer node(c);

    pass(bus, node, {on(60)});
    std::vector<MidiEvent> out = pass(bus, node, {off(60)}, true);
    TEST_ASSERT_EQUAL_UINT32(0, out.size());
    out = pass(bus, node, {}, false);
    TEST_ASSERT_EQUAL_UINT8(1, count(out, false));
}

// Switching from latch to sustain with the pedal up lets go of the latch.
static void test_switching_to_sustain_strands_nothing() {
    BusManager bus;
    NodeConfig c = buffer_config();
    NoteBuffer node(c);

    pass(bus, node, {on(60), off(60), on(64)});
    TEST_ASSERT_TRUE(node.set_param(NoteBuffer::P_MODE, NoteBuffer::MODE_SUSTAIN));
    std::vector<MidiEvent> out = pass(bus, node);
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    TEST_ASSERT_EQUAL_UINT8(60, out[0].data1);
    out = pass(bus, node, {off(64)});
    TEST_ASSERT_EQUAL_UINT8(1, count(out, false));
    TEST_ASSERT_FALSE(node.set_param(NoteBuffer::P_MODE, 2));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_descriptor_is_registered);
    RUN_TEST(test_a_note_is_sent_at_once_and_its_note_off_is_swallowed);
    RUN_TEST(test_a_note_already_held_is_not_struck_again);
    RUN_TEST(test_a_reset_edge_releases_everything_once);
    RUN_TEST(test_a_note_arriving_with_the_reset_survives_it);
    RUN_TEST(test_without_a_reset_only_silence_releases);
    RUN_TEST(test_the_channel_override_strands_nothing);
    RUN_TEST(test_what_is_not_a_note_passes_through);
    RUN_TEST(test_a_full_buffer_drops_the_new_note);
    RUN_TEST(test_sustain_with_the_pedal_up_is_a_through);
    RUN_TEST(test_sustain_without_a_gate_is_a_through);
    RUN_TEST(test_sustain_holds_while_the_pedal_is_down);
    RUN_TEST(test_sustain_catches_a_key_already_down);
    RUN_TEST(test_switching_to_sustain_strands_nothing);
    return UNITY_END();
}
