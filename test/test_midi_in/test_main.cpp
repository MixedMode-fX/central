#include <unity.h>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "midi/midi_queue.h"
#include "master.h"
#include "hal/midi_types.h"

void setUp() {}
void tearDown() {}

static MidiEvent note_on(uint8_t note, uint8_t channel = 1) {
    return MidiEvent{MIDI_NOTE_ON, channel, note, 100};
}

// ---------------------------------------------------------------------------
// The queue between the transports and the pass
// ---------------------------------------------------------------------------

static void test_queue_is_fifo_and_keeps_its_source() {
    MidiInputQueue q;
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_TRUE(q.push(mmMIDI_SERIAL_1, note_on(60)));
    TEST_ASSERT_TRUE(q.push(mmMIDI_USB_2, note_on(64)));
    TEST_ASSERT_EQUAL(2, q.count());

    SourcedMidiEvent out;
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL(mmMIDI_SERIAL_1, out.source);
    TEST_ASSERT_EQUAL(60, out.event.data1);
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL(mmMIDI_USB_2, out.source);
    TEST_ASSERT_EQUAL(64, out.event.data1);
    TEST_ASSERT_FALSE(q.pop(out));
}

// Overflow drops the newest and counts it, so "MIDI went strange under load"
// is a number rather than a mystery.
static void test_queue_overflow_is_counted() {
    MidiInputQueue q;
    uint8_t pushed = 0;
    while (q.push(mmMIDI_SERIAL_1, note_on(pushed))) pushed++;
    TEST_ASSERT_EQUAL(MIDI_INPUT_QUEUE_DEPTH - 1, pushed);
    TEST_ASSERT_EQUAL_UINT32(1, q.overflows());
    TEST_ASSERT_FALSE(q.push(mmMIDI_SERIAL_1, note_on(1)));
    TEST_ASSERT_EQUAL_UINT32(2, q.overflows());

    // The queue that is already there is intact and still in order.
    SourcedMidiEvent out;
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL(0, out.event.data1);
}

static void test_queue_wraps() {
    MidiInputQueue q;
    SourcedMidiEvent out;
    for (uint16_t i = 0; i < 1000; i++) {
        TEST_ASSERT_TRUE(q.push(mmMIDI_USB_0, note_on((uint8_t)(i % 100))));
        TEST_ASSERT_TRUE(q.pop(out));
        TEST_ASSERT_EQUAL(i % 100, out.event.data1);
    }
    TEST_ASSERT_EQUAL_UINT32(0, q.overflows());
    TEST_ASSERT_TRUE(q.empty());
}

// ---------------------------------------------------------------------------
// The input path through the master
// ---------------------------------------------------------------------------

// A note in on DIN 1, routed to USB cable 0, appears once there and nowhere
// else - the acceptance test for the whole input path.
static void test_din_in_to_usb_out_once_and_only_there() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_SERIAL_1, 0, 0};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    TEST_ASSERT_EQUAL(1, master.deliver_midi(mmMIDI_SERIAL_1, note_on(60)));
    master.pass(0);
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(mmMIDI_USB_0, midi.messages[0].target);
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, midi.messages[0].type);
    TEST_ASSERT_EQUAL(60, midi.messages[0].d1);

    master.pass(1000);                       // and only once
    TEST_ASSERT_EQUAL(1, midi.messages.size());
}

// Soft-thru is a patch, not a default. Nothing routed back to DIN 1 means
// nothing comes out of DIN 1 ...
static void test_unrouted_din_input_produces_no_din_output() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_SERIAL_1, 0, 0};      // in, going nowhere
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_SERIAL_1, note_on(60));
    master.pass(0);
    master.pass(1000);
    TEST_ASSERT_EQUAL(0, midi.messages.size());
}

// ... and the explicit thru patch - one input and one output on a shared bus -
// is what gives it back.
static void test_explicit_thru_patch_echoes() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_SERIAL_1, 0, 2};
    p.midi_out[0] = MidiOutConfig{mmMIDI_SERIAL_1, 0, 2};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_SERIAL_1, note_on(60));
    master.pass(0);
    TEST_ASSERT_EQUAL(1, midi.messages.size());
    TEST_ASSERT_EQUAL(mmMIDI_SERIAL_1, midi.messages[0].target);
    TEST_ASSERT_EQUAL(60, midi.messages[0].d1);
}

// One input event, two outputs: routing is bus assignment, so fan-out costs
// nothing.
static void test_fan_out_from_one_input_event() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_HOST_1, 0, 1};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 1};
    p.midi_out[1] = MidiOutConfig{mmMIDI_SERIAL_2, 0, 1};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_HOST_1, note_on(72));
    master.pass(0);
    TEST_ASSERT_EQUAL(2, midi.messages.size());
    TEST_ASSERT_EQUAL(mmMIDI_USB_0, midi.messages[0].target);
    TEST_ASSERT_EQUAL(mmMIDI_SERIAL_2, midi.messages[1].target);
}

// Several transports can feed one bus, and a port only takes what it is
// configured for.
static void test_source_and_channel_filters() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{(uint8_t)(mmMIDI_SERIAL_1 | mmMIDI_SERIAL_2), 0, 0};  // omni
    p.midi_in[1] = MidiInConfig{mmMIDI_USB_0, 5, 1};                                  // channel 5
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    TEST_ASSERT_EQUAL(1, master.deliver_midi(mmMIDI_SERIAL_2, note_on(60, 9)));
    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_USB_0, note_on(61, 4)));   // wrong channel
    TEST_ASSERT_EQUAL(1, master.deliver_midi(mmMIDI_USB_0, note_on(62, 5)));
    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_USB_3, note_on(63, 5)));   // wrong transport
    master.pass(0);
    TEST_ASSERT_EQUAL(1, master.buses().note_count(0));
    TEST_ASSERT_EQUAL(1, master.buses().note_count(1));
}

// Everything that is not a note shares the note bus: it is a MIDI event bus.
// A CC, a bend and an aftertouch message ride it unchanged.
static void test_non_note_messages_share_the_bus() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_USB_1, 0, 0};
    p.midi_out[0] = MidiOutConfig{mmMIDI_SERIAL_1, 0, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    master.deliver_midi(mmMIDI_USB_1, MidiEvent{MIDI_CONTROL_CHANGE, 1, 64, 127});
    master.deliver_midi(mmMIDI_USB_1, MidiEvent{MIDI_PITCH_BEND, 1, 0, 96});
    master.deliver_midi(mmMIDI_USB_1, MidiEvent{MIDI_AFTERTOUCH_CHANNEL, 1, 40, 0});
    master.deliver_midi(mmMIDI_USB_1, MidiEvent{MIDI_PROGRAM_CHANGE, 1, 7, 0});
    master.pass(0);
    TEST_ASSERT_EQUAL(4, midi.messages.size());
    TEST_ASSERT_EQUAL(MIDI_CONTROL_CHANGE, midi.messages[0].type);
    TEST_ASSERT_EQUAL(MIDI_PITCH_BEND, midi.messages[1].type);
    TEST_ASSERT_EQUAL(MIDI_AFTERTOUCH_CHANNEL, midi.messages[2].type);
    TEST_ASSERT_EQUAL(MIDI_PROGRAM_CHANGE, midi.messages[3].type);
}

// Realtime is transport-level: it reaches the clock and no bus, whatever the
// input ports are configured for.
static void test_realtime_never_reaches_a_note_bus() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_SERIAL_1, 0, 0};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();
    master.clock().set_source(MasterClock::CLOCK_MIDI);
    master.clock().start();

    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_CLOCK, 0, 0, 0}, 1000));
    TEST_ASSERT_EQUAL(0, master.deliver_midi(mmMIDI_SERIAL_1, MidiEvent{MIDI_STOP, 0, 0, 0}, 2000));
    master.pass(0);
    TEST_ASSERT_EQUAL(0, midi.messages.size());
    TEST_ASSERT_EQUAL(0, master.buses().note_count(0));
    TEST_ASSERT_FALSE(master.clock().running());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_queue_is_fifo_and_keeps_its_source);
    RUN_TEST(test_queue_overflow_is_counted);
    RUN_TEST(test_queue_wraps);
    RUN_TEST(test_din_in_to_usb_out_once_and_only_there);
    RUN_TEST(test_unrouted_din_input_produces_no_din_output);
    RUN_TEST(test_explicit_thru_patch_echoes);
    RUN_TEST(test_fan_out_from_one_input_event);
    RUN_TEST(test_source_and_channel_filters);
    RUN_TEST(test_non_note_messages_share_the_bus);
    RUN_TEST(test_realtime_never_reaches_a_note_bus);
    return UNITY_END();
}
