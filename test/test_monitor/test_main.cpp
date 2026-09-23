#include <unity.h>
#include <stdlib.h>
#include <vector>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "monitor/hal_tap.h"
#include "monitor/monitor.h"
#include "protocol/sysex.h"
#include "protocol/sysex_handler.h"
#include "patch/patch_manager.h"
#include "patch/default_patch.h"
#include "control/macros.h"
#include "control/cc_mapper.h"
#include "control/control_sum.h"
#include "control/mod_matrix.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "algorithm/midi/harmony.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// The monitor (monitor/monitor.h): the module's own record of what it is
// doing, kept for an editor that asks. Driven here as the three hosts drive
// it - a pass, then a sample - and read back the way an editor reads it, over
// the protocol.

static constexpr uint8_t CONTROL = MIDI_CONTROL_PORT;
static constexpr uint8_t KEYBOARD = mmMIDI_USB_0;
static constexpr uint32_t PASS_US = 1000;

struct Rig {
    FakeGpio gpio;
    RecordingMidiOut midi;
    FakeEeprom eeprom;
    FakeLeds led_driver;
    HalTap panel;
    MixedModeMaster master;
    StatusLeds leds;
    PatchStore store;
    PatchManager patches;
    Macros macros;
    CcMapper cc;
    ControlSum sum;
    ModMatrix mod;
    Monitor monitor;
    SysexHandler sysex;
    uint32_t now;

    Rig() : gpio(), midi(), eeprom(), led_driver(), panel(gpio, midi),
            master(panel, panel), leds(led_driver), store(eeprom),
            patches(master, store, leds), macros(), cc(patches, master, macros), sum(cc), mod(patches, cc, sum),
            monitor(master, panel, leds),
            sysex(patches, master, store, leds, panel, cc, mod, macros, sum, monitor), now(0) {}

    // One loop of main.cpp, as far as the monitor is concerned: the pass, the
    // LEDs, and then the record.
    void pass() {
        master.pass(now);
        leds.service(now);
        monitor.sample(now);
        now += PASS_US;
    }
    void passes(uint32_t n) { for (uint32_t i = 0; i < n; i++) pass(); }

    void load(const Patch& p) {
        GlobalSettings g = default_globals();
        TEST_ASSERT_EQUAL(APPLY_OK, patches.apply(p, g, now));
    }

    // A request, framed the way the wire carries it: the note bus mask, and
    // the nodes whose positions are wanted.
    void request(uint16_t note_buses, const std::vector<uint8_t>& nodes = {}) {
        std::vector<uint8_t> m = {0xF0, SYSEX_MANUFACTURER, sysex.device_id(),
                                  SYSEX_MONITOR_REQUEST, SYSEX_PROTOCOL_VERSION,
                                  (uint8_t)(note_buses & 0x7F), (uint8_t)((note_buses >> 7) & 0x7F),
                                  (uint8_t)((note_buses >> 14) & 0x7F), (uint8_t)nodes.size()};
        for (uint8_t n : nodes) m.push_back(n);
        m.push_back(0xF7);
        midi.clear();
        sysex.deliver_sysex(CONTROL, m.data(), (uint16_t)m.size(), now);
    }
    const RecordingMidiOut::Sysex* frame() const { return midi.last_reply(SYSEX_MONITOR); }
};

// The frame, decoded the way the app decodes it (app/src/protocol/device.js).
struct Frame {
    uint32_t at = 0, count = 0, gate_now = 0, gate_since = 0;
    bool running = false;
    uint16_t bpm = 0, jack_in_now = 0, jack_in_since = 0, jack_out_now = 0, jack_out_since = 0, green = 0, red = 0;
    int16_t cv[N_CV_BUS] = {};
    struct Node { uint8_t node = 0; std::vector<uint8_t> values = {}; };
    std::vector<Node> nodes = {};
    bool lost = false;
    struct Event { uint8_t where = 0, arg = 0, type = 0, channel = 0, d1 = 0, d2 = 0; uint16_t age_ms = 0; };
    std::vector<Event> events = {};
};

static Frame decode(const RecordingMidiOut::Sysex* s) {
    TEST_ASSERT_NOT_NULL(s);
    const std::vector<uint8_t>& b = s->bytes;
    size_t at = 5;
    const auto u8 = [&]{ return b[at++]; };
    const auto u14 = [&]{ uint16_t v = (uint16_t)(b[at] | (b[at + 1] << 7)); at += 2; return v; };
    const auto u32 = [&]{ uint32_t v = 0; for (int i = 0; i < 5; i++) v |= (uint32_t)b[at++] << (7 * i); return v; };
    const auto s14 = [&]{ uint16_t m = u14(); return (int16_t)(u8() ? -(int32_t)m : (int32_t)m); };
    Frame f;
    f.at = u32();
    f.running = (u8() & 1) != 0;
    f.bpm = u14();
    f.count = u32();
    f.gate_now = u32(); f.gate_since = u32();
    f.jack_in_now = u14(); f.jack_in_since = u14(); f.jack_out_now = u14(); f.jack_out_since = u14();
    f.green = u14(); f.red = u14();
    for (uint8_t c = 0; c < N_CV_BUS; c++) f.cv[c] = s14();
    const uint8_t n_nodes = u8();
    for (uint8_t i = 0; i < n_nodes; i++) {
        Frame::Node n; n.node = u8();
        const uint8_t m = u8();
        for (uint8_t v = 0; v < m; v++) n.values.push_back(u8());
        f.nodes.push_back(n);
    }
    f.lost = u8() != 0;
    const uint8_t n_events = u8();
    for (uint8_t i = 0; i < n_events; i++) {
        Frame::Event e;
        const uint8_t where = u8();
        e.where = (uint8_t)(where & 0x01);
        e.arg = (uint8_t)(u8() | ((where & 0x40) ? 0x80u : 0u));
        const uint8_t flags = u8();
        e.type = (flags & 0x40) ? MIDI_NOTE_ON : MIDI_NOTE_OFF;
        e.channel = (uint8_t)(flags & 0x1F);
        e.d1 = u8(); e.d2 = u8();
        e.age_ms = u14();
        f.events.push_back(e);
    }
    TEST_ASSERT_EQUAL(0xF7, b[at]);
    return f;
}

// Jack 1 in -> gate bus 0 -> jack 2 out; a keyboard on USB 1 -> note bus 0 ->
// USB 1 out: one gate path and one note path through the module.
static Patch through_patch() {
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, one_bus(0)};
    p.gate_ports[1] = GatePortConfig{GATE_PORT_OUT, one_bus(0)};
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, one_bus(0)};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_1, 0, one_bus(0)};
    return p;
}

static void note(Rig& rig, uint8_t type, uint8_t pitch, uint8_t velocity) {
    const MidiEvent e = {type, 1, pitch, velocity};
    rig.master.deliver_midi(KEYBOARD, e, rig.now);
}

// --- armed by a request ------------------------------------------------------

static void test_nothing_is_recorded_until_somebody_asks() {
    Rig rig;
    rig.load(through_patch());
    rig.gpio.set_input(0, GPIO_HIGH);
    note(rig, MIDI_NOTE_ON, 60, 100);
    rig.passes(3);
    TEST_ASSERT_FALSE(rig.monitor.armed(rig.now));
    TEST_ASSERT_EQUAL(0, rig.monitor.gate_since());
    TEST_ASSERT_EQUAL(0, rig.monitor.event_count());
    // The module did its work regardless: the jack and the cable moved.
    TEST_ASSERT_EQUAL(GPIO_HIGH, rig.gpio.outputs[1]);
    TEST_ASSERT_EQUAL(1, rig.midi.messages.size());
}

static void test_a_request_arms_the_monitor_and_it_disarms_itself() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0x0001);
    TEST_ASSERT_TRUE(rig.monitor.armed(rig.now));
    rig.gpio.set_input(0, GPIO_HIGH);
    rig.pass();
    TEST_ASSERT_EQUAL(1, rig.monitor.gate_since());
    // A second of silence, and it is a module nobody is watching again.
    rig.now += MONITOR_ARMED_US;
    rig.pass();
    TEST_ASSERT_FALSE(rig.monitor.armed(rig.now));
    rig.request(0x0001);
    // Armed again, and from an empty record: what happened while nobody
    // was asking is not the answer to a new question.
    rig.gpio.set_input(0, GPIO_LOW);
    rig.pass();
    TEST_ASSERT_EQUAL(0, rig.monitor.gate_since());
}

// --- the fold ------------------------------------------------------------------

// A trigger on this machine is high for a pass or two, and an editor paints
// sixteen passes later. `since` is what keeps it visible; `now` is what lets a
// held level read as held.
static void test_a_pulse_shorter_than_a_frame_is_kept_until_the_frame_is_read() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0);
    rig.gpio.set_input(0, GPIO_HIGH);
    rig.passes(2);
    rig.gpio.set_input(0, GPIO_LOW);
    rig.passes(20);
    const Frame f = decode((rig.request(0), rig.frame()));
    TEST_ASSERT_EQUAL(0x0001, f.gate_since);
    TEST_ASSERT_EQUAL(0, f.gate_now);
    TEST_ASSERT_EQUAL(0x0001, f.jack_in_since);
    TEST_ASSERT_EQUAL(0x0002, f.jack_out_since);
    TEST_ASSERT_EQUAL(0, f.jack_out_now);
    // Read once: the next frame starts from what is high now, which is nothing.
    rig.passes(5);
    const Frame g = decode((rig.request(0), rig.frame()));
    TEST_ASSERT_EQUAL(0, g.gate_since);
    TEST_ASSERT_EQUAL(0, g.jack_out_since);
    TEST_ASSERT_TRUE(g.at > f.at);
}

static void test_a_level_held_across_frames_stays_lit() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0);
    rig.gpio.set_input(0, GPIO_HIGH);
    rig.passes(20);
    Frame f = decode((rig.request(0), rig.frame()));
    TEST_ASSERT_EQUAL(1, f.gate_now);
    rig.passes(20);
    f = decode((rig.request(0), rig.frame()));
    TEST_ASSERT_EQUAL(1, f.gate_since);
    TEST_ASSERT_EQUAL(1, f.gate_now);
}

static void test_the_frame_carries_the_leds_and_the_clock() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0);
    rig.master.clock().set_bpm(97);
    rig.master.clock().start();
    rig.leds.beat(rig.now);                      // the green flash a downbeat makes
    rig.passes(3);
    const Frame f = decode((rig.request(0), rig.frame()));
    TEST_ASSERT_TRUE(f.running);
    TEST_ASSERT_EQUAL(97, f.bpm);
    TEST_ASSERT_EQUAL(StatusLeds::BRIGHT, f.green);
    // The peak since the last frame, not the level now: a forty-millisecond
    // flash is over long before a slow editor asks again.
    rig.now += StatusLeds::BEAT_FLASH_US * 2;
    rig.passes(2);
    TEST_ASSERT_TRUE(rig.leds.level(LED_GREEN) < StatusLeds::BRIGHT);
    TEST_ASSERT_EQUAL(StatusLeds::BRIGHT, decode((rig.request(0), rig.frame())).green);
    rig.passes(2);
    TEST_ASSERT_TRUE(decode((rig.request(0), rig.frame())).green < StatusLeds::BRIGHT);
}

// --- the events ---------------------------------------------------------------

static void test_a_note_on_a_watched_bus_is_reported_once_with_its_age() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0x0001);
    note(rig, MIDI_NOTE_ON, 60, 100);
    rig.pass();
    rig.passes(9);
    note(rig, MIDI_NOTE_OFF, 60, 0);
    rig.pass();
    const Frame f = decode((rig.request(0x0001), rig.frame()));
    // The bus, and the cable: the same note at the two points it was seen.
    TEST_ASSERT_EQUAL(4, f.events.size());
    const Frame::Event& on_bus = f.events[0];
    const Frame::Event& out = f.events[1];
    TEST_ASSERT_EQUAL(Monitor::WHERE_BUS, on_bus.where);
    TEST_ASSERT_EQUAL(0, on_bus.arg);
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, on_bus.type);
    TEST_ASSERT_EQUAL(1, on_bus.channel);
    TEST_ASSERT_EQUAL(60, on_bus.d1);
    TEST_ASSERT_EQUAL(100, on_bus.d2);
    TEST_ASSERT_EQUAL(10, on_bus.age_ms);      // ten passes before the frame's end
    TEST_ASSERT_EQUAL(Monitor::WHERE_OUT, out.where);
    TEST_ASSERT_EQUAL(mmMIDI_USB_1, out.arg);
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, f.events[2].type);
    TEST_ASSERT_EQUAL(0, f.events[2].age_ms);
    TEST_ASSERT_FALSE(f.lost);
    // Taken: the next frame has none of them.
    rig.pass();
    TEST_ASSERT_EQUAL(0, decode((rig.request(0x0001), rig.frame())).events.size());
}

static void test_a_bus_nobody_watches_is_not_read() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0x0002);                          // bus 1, not the bus the notes are on
    note(rig, MIDI_NOTE_ON, 60, 100);
    rig.pass();
    const Frame f = decode((rig.request(0x0002), rig.frame()));
    TEST_ASSERT_EQUAL(1, f.events.size());
    TEST_ASSERT_EQUAL(Monitor::WHERE_OUT, f.events[0].where);
}

static void test_the_host_port_survives_the_seven_bit_wire() {
    Rig rig;
    Patch p = through_patch();
    p.midi_out[0] = MidiOutConfig{mmMIDI_HOST_1, 0, one_bus(0)};
    rig.load(p);
    rig.request(0);
    note(rig, MIDI_NOTE_ON, 60, 100);
    rig.pass();
    const Frame f = decode((rig.request(0), rig.frame()));
    TEST_ASSERT_EQUAL(1, f.events.size());
    TEST_ASSERT_EQUAL(mmMIDI_HOST_1, f.events[0].arg);
}

static void test_a_burst_past_the_record_is_reported_as_lost_not_stalled() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0x0001);
    for (uint8_t i = 0; i < MONITOR_EVENTS; i++) {
        note(rig, MIDI_NOTE_ON, (uint8_t)(40 + i), 100);
        rig.pass();
    }
    // Every note still left the module: the record is a record, not a gate.
    TEST_ASSERT_EQUAL(MONITOR_EVENTS, rig.midi.messages.size());
    const Frame f = decode((rig.request(0x0001), rig.frame()));
    TEST_ASSERT_EQUAL(MONITOR_EVENTS, f.events.size());
    TEST_ASSERT_TRUE(f.lost);
    TEST_ASSERT_TRUE(f.events.size() * 7 + 100 <= SYSEX_TX_MAX);
    rig.pass();
    TEST_ASSERT_FALSE(decode((rig.request(0x0001), rig.frame())).lost);
}

// --- where a node is ----------------------------------------------------------

static void test_positions_are_read_for_the_nodes_the_request_names() {
    Rig rig;
    Patch p = through_patch();
    p.nodes[0] = node_config(ALGO_STEP_SEQ);
    p.nodes[0].in_buses[0] = one_bus(0);          // advance from the jack
    p.nodes[0].out_buses[0] = one_bus(2);
    p.nodes[0].params[0] = 4;
    p.nodes[0].params[3] = 0x0F;
    p.nodes[1] = node_config(ALGO_HARMONY);
    p.nodes[1].in_buses[0] = one_bus(0);
    p.nodes[1].out_buses[0] = one_bus(1);
    p.nodes[1].params[Harmony::P_LOOP] = 3;
    p.nodes[2] = node_config(ALGO_TRANSPOSE);     // has no position at all
    p.nodes[2].in_buses[0] = one_bus(0);
    p.nodes[2].out_buses[0] = one_bus(3);
    p.n_nodes = 3;
    rig.load(p);
    rig.request(0);
    rig.pass();

    Frame f = decode((rig.request(0, {0, 1, 2}), rig.frame()));
    TEST_ASSERT_EQUAL(3, f.nodes.size());
    // Before the first advance there is nothing to point at, and 0x7F says
    // so on the wire where the node's 0xFF could not.
    TEST_ASSERT_EQUAL(1, f.nodes[0].values.size());
    TEST_ASSERT_EQUAL(0x7F, f.nodes[0].values[0]);
    TEST_ASSERT_EQUAL(2 + 3, f.nodes[1].values.size());
    TEST_ASSERT_EQUAL(0x7F, f.nodes[1].values[0]);
    TEST_ASSERT_EQUAL(0, f.nodes[2].values.size());

    // Two edges on the jack: the sequencer is on a step, the harmony on a
    // degree of the key, and the loop is being written down.
    for (int edge = 0; edge < 2; edge++) {
        rig.gpio.set_input(0, GPIO_HIGH); rig.passes(2);
        rig.gpio.set_input(0, GPIO_LOW); rig.passes(2);
    }
    f = decode((rig.request(0, {0, 1}), rig.frame()));
    TEST_ASSERT_EQUAL(1, f.nodes[0].values[0]);
    TEST_ASSERT_TRUE(f.nodes[1].values[0] < Harmony::DEGREES);
    TEST_ASSERT_EQUAL(1, f.nodes[1].values[1]);
    TEST_ASSERT_TRUE(f.nodes[1].values[2] < Harmony::DEGREES);
    TEST_ASSERT_EQUAL(0x7F, f.nodes[1].values[4]);   // the slot not reached yet

    // A node the patch has not got answers with no values rather than a NAK:
    // an editor's patch may be a step ahead of the module's.
    f = decode((rig.request(0, {40}), rig.frame()));
    TEST_ASSERT_EQUAL(1, f.nodes.size());
    TEST_ASSERT_EQUAL(0, f.nodes[0].values.size());
}

static void test_a_drum_sequencer_answers_a_step_per_lane() {
    Rig rig;
    Patch p = through_patch();
    p.nodes[0] = node_config(ALGO_DRUM_SEQ_GATE);
    p.nodes[0].in_buses[0] = one_bus(0);
    p.nodes[0].out_buses[0] = one_bus(2);
    p.n_nodes = 1;
    rig.load(p);
    rig.request(0);
    rig.gpio.set_input(0, GPIO_HIGH); rig.passes(2);
    const Frame f = decode((rig.request(0, {0}), rig.frame()));
    TEST_ASSERT_EQUAL(DRUM_SEQ_LANES, f.nodes[0].values.size());
    for (uint8_t lane = 0; lane < DRUM_SEQ_LANES; lane++) TEST_ASSERT_EQUAL(0, f.nodes[0].values[lane]);
}

// --- the wire ------------------------------------------------------------------

static void test_a_request_naming_too_many_nodes_is_refused() {
    Rig rig;
    rig.load(through_patch());
    std::vector<uint8_t> nodes;
    for (uint8_t i = 0; i <= MONITOR_MAX_NODES; i++) nodes.push_back(i);
    rig.request(0, nodes);
    TEST_ASSERT_NULL(rig.frame());
    const RecordingMidiOut::Sysex* nak = rig.midi.last_reply(SYSEX_NAK);
    TEST_ASSERT_NOT_NULL(nak);
    TEST_ASSERT_EQUAL(SYSEX_ERR_BAD_ARGUMENT, nak->bytes[5]);

    // And a request that promises nodes it does not carry.
    uint8_t short_one[] = {0xF0, SYSEX_MANUFACTURER, SYSEX_DEFAULT_DEVICE, SYSEX_MONITOR_REQUEST,
                           SYSEX_PROTOCOL_VERSION, 0, 0, 0, 2, 0xF7};
    rig.midi.clear();
    rig.sysex.deliver_sysex(CONTROL, short_one, sizeof short_one, rig.now);
    nak = rig.midi.last_reply(SYSEX_NAK);
    TEST_ASSERT_NOT_NULL(nak);
    TEST_ASSERT_EQUAL(SYSEX_ERR_TRUNCATED, nak->bytes[5]);
}

static void test_the_fullest_frame_fits_one_reply() {
    Rig rig;
    Patch p = through_patch();
    for (uint8_t i = 0; i < MONITOR_MAX_NODES; i++) {
        p.nodes[i] = node_config(ALGO_HARMONY);
        p.nodes[i].in_buses[0] = one_bus(0);
        p.nodes[i].out_buses[0] = one_bus((uint8_t)(1 + i));
        p.nodes[i].params[Harmony::P_LOOP] = Harmony::MAX_PHRASE;
    }
    p.n_nodes = MONITOR_MAX_NODES;
    rig.load(p);
    rig.request(0x0001);
    for (uint8_t i = 0; i < MONITOR_EVENTS + 4; i++) { note(rig, MIDI_NOTE_ON, (uint8_t)(40 + i), 100); rig.pass(); }
    std::vector<uint8_t> nodes;
    for (uint8_t i = 0; i < MONITOR_MAX_NODES; i++) nodes.push_back(i);
    rig.request(0x0001, nodes);
    const RecordingMidiOut::Sysex* s = rig.frame();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->bytes.size() <= SYSEX_TX_MAX);
    const Frame f = decode(s);
    TEST_ASSERT_EQUAL(MONITOR_MAX_NODES, f.nodes.size());
    TEST_ASSERT_EQUAL(MONITOR_EVENTS, f.events.size());
    for (const Frame::Node& n : f.nodes) TEST_ASSERT_EQUAL(2 + Harmony::MAX_PHRASE, n.values.size());
}

static void test_watching_never_allocates() {
    Rig rig;
    rig.load(through_patch());
    rig.request(0x0001);
    rig.midi.clear();
    rig.midi.sysex.reserve(4096);
    uint8_t message[] = {0xF0, SYSEX_MANUFACTURER, SYSEX_DEFAULT_DEVICE, SYSEX_MONITOR_REQUEST,
                         SYSEX_PROTOCOL_VERSION, 1, 0, 0, 0, 0xF7};
    const size_t before = g_allocations;
    for (int i = 0; i < 50; i++) {
        rig.gpio.set_input(0, (i & 1) ? GPIO_HIGH : GPIO_LOW);
        note(rig, (i & 1) ? MIDI_NOTE_ON : MIDI_NOTE_OFF, 60, (i & 1) ? 100 : 0);
        rig.passes(4);
        rig.sysex.deliver_sysex(CONTROL, message, sizeof message, rig.now);
    }
    // The only allocations left are the fake's own: it copies each reply
    // into a byte vector so a test can read it. One per frame, and no more.
    TEST_ASSERT_EQUAL(rig.midi.sysex.size(), g_allocations - before);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_recorded_until_somebody_asks);
    RUN_TEST(test_a_request_arms_the_monitor_and_it_disarms_itself);
    RUN_TEST(test_a_pulse_shorter_than_a_frame_is_kept_until_the_frame_is_read);
    RUN_TEST(test_a_level_held_across_frames_stays_lit);
    RUN_TEST(test_the_frame_carries_the_leds_and_the_clock);
    RUN_TEST(test_a_note_on_a_watched_bus_is_reported_once_with_its_age);
    RUN_TEST(test_a_bus_nobody_watches_is_not_read);
    RUN_TEST(test_the_host_port_survives_the_seven_bit_wire);
    RUN_TEST(test_a_burst_past_the_record_is_reported_as_lost_not_stalled);
    RUN_TEST(test_positions_are_read_for_the_nodes_the_request_names);
    RUN_TEST(test_a_drum_sequencer_answers_a_step_per_lane);
    RUN_TEST(test_a_request_naming_too_many_nodes_is_refused);
    RUN_TEST(test_the_fullest_frame_fits_one_reply);
    RUN_TEST(test_watching_never_allocates);
    return UNITY_END();
}
