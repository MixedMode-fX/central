#include <unity.h>
#include <stdlib.h>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "control/cc_mapper.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "patch/default_patch.h"
#include "protocol/sysex_handler.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "algorithm/sequencer/sequencers.h"
#include "algorithm/midi/transpose.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// MIDI CC mapping (#21): the feature that makes the module playable.

static constexpr uint8_t KEYBOARD = mmMIDI_SERIAL_1;   // the port a controller is on

struct Rig {
    FakeGpio gpio;
    RecordingMidiOut midi;
    FakeEeprom eeprom;
    FakeLeds led_driver;
    MixedModeMaster master;
    StatusLeds leds;
    PatchStore store;
    PatchManager patches;
    CcMapper cc;
    SysexHandler sysex;

    Rig() : gpio(), midi(), eeprom(), led_driver(),
            master(gpio, midi), leds(led_driver), store(eeprom),
            patches(master, store, leds), cc(patches, master),
            sysex(patches, master, store, leds, midi, cc) {}

    // One CC through the whole input path: offered to the mapping table, and
    // delivered to the graph only if it was not consumed. This is exactly
    // what main.cpp's loop does.
    bool feed_cc(uint8_t cc_number, uint8_t value, uint8_t channel = 1,
                 uint8_t source = KEYBOARD, uint32_t now_us = 0) {
        if (cc.observe(source, channel, cc_number, value, now_us)) return true;
        const MidiEvent e = {MIDI_CONTROL_CHANGE, channel, cc_number, value};
        master.deliver_midi(source, e, now_us);
        return false;
    }

    void pass(uint32_t now_us) { cc.apply(now_us); master.pass(now_us); }
};

// A patch with something worth turning: a Euclidean sequencer and a transpose.
static Patch mappable_patch() {
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, 0};
    p.nodes[0] = node_config(ALGO_EUCLID_SEQ);
    p.nodes[0].in_bus[0] = 0;              // advance, gate bus 0
    p.nodes[0].out_bus[0] = 1;
    p.nodes[0].params[0] = 8;              // eight steps
    p.nodes[0].params[3] = 3;              // E(3,8)
    p.nodes[1] = node_config(ALGO_TRANSPOSE);
    p.nodes[1].in_bus[0] = 0;              // note bus 0
    p.nodes[1].out_bus[0] = 1;
    p.n_nodes = 2;
    p.midi_out[0] = MidiOutConfig{KEYBOARD, 0, 1};
    return p;
}

static CcMapping node_mapping(uint8_t cc_number, uint8_t node, uint16_t param,
                              uint16_t lo = 0, uint16_t hi = 0, uint8_t flags = 0) {
    CcMapping m = unused_mapping();
    m.source_mask = KEYBOARD;
    m.channel = 1;
    m.cc = cc_number;
    m.target_kind = CC_TARGET_NODE;
    m.target_index = node;
    m.param = param;
    m.min = lo;
    m.max = hi;
    m.flags = flags;
    return m;
}

static void install(Rig& rig, const CcMapping& m, uint8_t slot = 0) {
    Patch p = mappable_patch();
    p.cc_map[slot] = m;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));
    rig.cc.reset();
}

// ---------------------------------------------------------------------------
// The basic contract: a mapped CC moves its target, an unmapped one does not.
// ---------------------------------------------------------------------------
static void test_a_mapped_cc_moves_its_parameter() {
    Rig rig;
    // Euclid's pulse count, over its real range 0..MAX_SEQUENCE_LEN.
    install(rig, node_mapping(20, 0, 3));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    TEST_ASSERT_EQUAL(3, seq->pulses());

    rig.feed_cc(20, 127);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());

    rig.feed_cc(20, 0);
    rig.pass(2000);
    TEST_ASSERT_EQUAL(0, seq->pulses());

    rig.feed_cc(20, 64);
    rig.pass(3000);
    TEST_ASSERT_EQUAL(16, seq->pulses());        // half of 0..32, rounded
}

static void test_the_wrong_channel_port_or_number_changes_nothing() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    rig.feed_cc(21, 127);                        // wrong CC number
    rig.pass(1000);
    TEST_ASSERT_EQUAL(3, seq->pulses());

    rig.feed_cc(20, 127, 5);                     // wrong channel
    rig.pass(2000);
    TEST_ASSERT_EQUAL(3, seq->pulses());

    rig.feed_cc(20, 127, 1, mmMIDI_SERIAL_2);    // wrong port
    rig.pass(3000);
    TEST_ASSERT_EQUAL(3, seq->pulses());

    rig.feed_cc(20, 127, 1, KEYBOARD);           // and now the right one
    rig.pass(4000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());
}

// Channel 0 is omni, the way MidiInConfig already means it.
static void test_omni_matches_every_channel() {
    Rig rig;
    CcMapping m = node_mapping(20, 0, 3);
    m.channel = 0;
    install(rig, m);
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    rig.feed_cc(20, 127, 11);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());
}

// The acceptance case from #21: driven end to end from a MidiEvent, the
// pattern changes and the step cursor does not.
static void test_a_cc_on_euclid_changes_the_pattern_without_moving_the_cursor() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint32_t now = 0;
    for (int i = 0; i < 4; i++) { rig.pass(now); now += 1000; }
    const uint8_t cursor = seq->position();
    const uint32_t steps = seq->steps_taken();
    const uint32_t before = seq->pattern();

    rig.feed_cc(20, 20);                         // 20/127 of 0..32 -> 5
    rig.pass(now);
    TEST_ASSERT_EQUAL(5, seq->pulses());
    TEST_ASSERT_NOT_EQUAL(before, seq->pattern());
    TEST_ASSERT_EQUAL(cursor, seq->position());
    TEST_ASSERT_EQUAL(steps, seq->steps_taken());
}

// A sub-range: the knob sweeps only part of the parameter, in the
// parameter's own units.
static void test_a_sub_range_limits_the_sweep() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 4, 8));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    rig.feed_cc(20, 0);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(4, seq->pulses());
    rig.feed_cc(20, 127);
    rig.pass(2000);
    TEST_ASSERT_EQUAL(8, seq->pulses());
}

// ---------------------------------------------------------------------------
// Pass-through: do not silently eat the CC, and do not silently forward it.
// ---------------------------------------------------------------------------
static void test_a_mapped_cc_is_consumed_by_default() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3));
    TEST_ASSERT_TRUE(rig.feed_cc(20, 100));
    rig.pass(1000);
    for (const auto& m : rig.midi.messages) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(MIDI_CONTROL_CHANGE, m.type,
                                      "a consumed CC must not reach a note bus");
    }
}

static void test_a_pass_through_cc_reaches_the_graph_as_well() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 0, 0, CC_PASS_THROUGH));
    TEST_ASSERT_FALSE(rig.feed_cc(20, 100));
    // Two passes: an event delivered between passes lands in the back buffer
    // and is published by the first pass's swap, so the graph sees it on the
    // second - the same shape every other input test uses.
    rig.pass(1000);
    rig.pass(2000);

    bool forwarded = false;
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_CONTROL_CHANGE && m.d1 == 20 && m.d2 == 100) forwarded = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(forwarded, "a pass-through CC must reach a MidiOutPort");
    // And it still moved the parameter.
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    TEST_ASSERT_EQUAL(25, seq->pulses());
}

// A Sustain node writes CC onto a note bus; mapping only reads *incoming*
// transport events, so the two do not interact.
static void test_a_sustain_output_is_not_affected_by_a_mapping() {
    Rig rig;
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.nodes[0] = node_config(ALGO_SUSTAIN);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[1] = 20;                    // it emits CC 20
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{KEYBOARD, 0, 0};
    // ... and CC 20 is also bound to the node's own controller parameter.
    p.cc_map[0] = node_mapping(20, 0, 1, 0, 0, 0);
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));

    uint32_t now = 0;
    rig.gpio.set_input(0, true);
    for (int i = 0; i < 20; i++) { rig.pass(now); now += 1000; }

    bool emitted = false;
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_CONTROL_CHANGE && m.d1 == 20) emitted = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(emitted, "the pedal's CC goes out however the mapping is set");
}

// ---------------------------------------------------------------------------
// 14-bit: tempo does not fit in seven bits, and this is the general problem.
// ---------------------------------------------------------------------------
static void test_fourteen_bit_tempo_resolves_the_whole_range() {
    Rig rig;
    CcMapping m = unused_mapping();
    m.source_mask = KEYBOARD;
    m.channel = 1;
    m.cc = 10;                                    // MSB; CC 42 is the LSB
    m.target_kind = CC_TARGET_CLOCK;
    m.param = CC_CLOCK_TEMPO;
    m.flags = CC_FOURTEEN_BIT;
    install(rig, m);

    // Bottom of the range.
    rig.feed_cc(42, 0);
    rig.feed_cc(10, 0);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(CLOCK_MIN_BPM, rig.master.clock().bpm());

    // Top of it.
    rig.feed_cc(42, 127);
    rig.feed_cc(10, 127);
    rig.pass(2000);
    TEST_ASSERT_EQUAL(CLOCK_MAX_BPM, rig.master.clock().bpm());

    // And a resolution seven bits could not reach: one LSB step is far less
    // than the 2.2 BPM a 7-bit mapping would give.
    rig.feed_cc(42, 0);
    rig.feed_cc(10, 64);
    rig.pass(3000);
    const uint16_t a = rig.master.clock().bpm();
    rig.feed_cc(42, 40);
    rig.feed_cc(10, 64);
    rig.pass(4000);
    const uint16_t b = rig.master.clock().bpm();
    TEST_ASSERT_TRUE_MESSAGE(b > a && (b - a) < 2, "14-bit tempo must resolve finer than 2 BPM");
}

// An MSB with no LSB is applied rather than stalling, and the one-message
// latency of an LSB-first controller is exactly one message.
static void test_a_lone_msb_is_applied_and_does_not_stall() {
    Rig rig;
    CcMapping m = unused_mapping();
    m.source_mask = KEYBOARD;
    m.channel = 1;
    m.cc = 10;
    m.target_kind = CC_TARGET_CLOCK;
    m.param = CC_CLOCK_TEMPO;
    m.flags = CC_FOURTEEN_BIT;
    install(rig, m);

    rig.feed_cc(10, 127);                         // no LSB has ever arrived
    rig.pass(1000);
    TEST_ASSERT_TRUE(rig.master.clock().bpm() > CLOCK_MIN_BPM);

    // An LSB on its own writes nothing; the next MSB picks it up.
    const uint16_t before = rig.master.clock().bpm();
    rig.feed_cc(42, 127);
    rig.pass(2000);
    TEST_ASSERT_EQUAL(before, rig.master.clock().bpm());
    rig.feed_cc(10, 127);
    rig.pass(3000);
    TEST_ASSERT_EQUAL(CLOCK_MAX_BPM, rig.master.clock().bpm());
}

// ---------------------------------------------------------------------------
// Takeover: the failure everyone hits, in three modes.
// ---------------------------------------------------------------------------
static void test_jump_takes_the_value_immediately() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 0, 0, CC_TAKEOVER_JUMP));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    rig.feed_cc(20, 127);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());

    // A SysEx edit moves the target out from under the knob.
    rig.patches.set_param(0, 3, 2, 2000);
    TEST_ASSERT_EQUAL(2, seq->pulses());

    // The knob jumps straight back. Loud, and always responsive.
    rig.feed_cc(20, 127);
    rig.pass(3000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());
}

static void test_pickup_waits_until_the_knob_crosses_the_value() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 0, 0, CC_TAKEOVER_PICKUP));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    // Establish where the knob is: near the bottom.
    rig.feed_cc(20, 8);
    rig.pass(1000);
    // Push the target to the top behind the knob's back.
    rig.patches.set_param(0, 3, 30, 2000);
    TEST_ASSERT_EQUAL(30, seq->pulses());

    // Moving the knob a little does nothing: it has not reached 30 yet.
    rig.feed_cc(20, 20);
    rig.pass(3000);
    TEST_ASSERT_EQUAL(30, seq->pulses());

    // Sweeping it past the target picks it up.
    rig.feed_cc(20, 127);
    rig.pass(4000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());

    // And from then on it tracks normally.
    rig.feed_cc(20, 0);
    rig.pass(5000);
    TEST_ASSERT_EQUAL(0, seq->pulses());
}

static void test_scale_reaches_both_ends_from_wherever_the_knob_is() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 0, 0, CC_TAKEOVER_SCALE));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    rig.feed_cc(20, 64);                          // the knob is at the middle
    rig.pass(1000);
    rig.patches.set_param(0, 3, 8, 2000);         // the target is at 8
    TEST_ASSERT_EQUAL(8, seq->pulses());

    // All the way up: the remaining travel still reaches the top.
    rig.feed_cc(20, 127);
    rig.pass(3000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());

    // And a fresh scale mapping from the middle reaches the bottom too.
    Rig other;
    install(other, node_mapping(20, 0, 3, 0, 0, CC_TAKEOVER_SCALE));
    EuclidianSequencer* s2 = static_cast<EuclidianSequencer*>(other.master.node(0));
    other.feed_cc(20, 64);
    other.pass(1000);
    other.patches.set_param(0, 3, 8, 2000);
    other.feed_cc(20, 0);
    other.pass(3000);
    TEST_ASSERT_EQUAL(0, s2->pulses());
}

// ---------------------------------------------------------------------------
// Relative encoders: three incompatible encodings, all of which have to work.
// ---------------------------------------------------------------------------
static void increments_and_decrements(uint8_t flags, uint8_t up, uint8_t down, const char* what) {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 0, 0, flags));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    rig.patches.set_param(0, 3, 10, 0);

    rig.feed_cc(20, up);
    rig.pass(1000);
    TEST_ASSERT_EQUAL_MESSAGE(11, seq->pulses(), what);

    rig.feed_cc(20, down);
    rig.pass(2000);
    rig.feed_cc(20, down);
    rig.pass(3000);
    TEST_ASSERT_EQUAL_MESSAGE(9, seq->pulses(), what);

    // And it clamps rather than wrapping at the ends.
    for (int i = 0; i < 40; i++) { rig.feed_cc(20, down); rig.pass((uint32_t)(4000 + i)); }
    TEST_ASSERT_EQUAL_MESSAGE(0, seq->pulses(), what);
    for (int i = 0; i < 60; i++) { rig.feed_cc(20, up); rig.pass((uint32_t)(9000 + i)); }
    TEST_ASSERT_EQUAL_MESSAGE(MAX_SEQUENCE_LEN, seq->pulses(), what);
}

static void test_relative_twos_complement() { increments_and_decrements(CC_RELATIVE_TWOS, 1, 127, "two's complement"); }
static void test_relative_signed_bit()      { increments_and_decrements(CC_RELATIVE_SIGNED, 1, 65, "signed bit"); }
static void test_relative_offset_64()       { increments_and_decrements(CC_RELATIVE_OFFSET64, 65, 63, "offset-64"); }

// The value that means "no movement" in each encoding writes nothing.
static void test_a_relative_zero_writes_nothing() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3, 0, 0, CC_RELATIVE_TWOS));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    const uint32_t before = rig.cc.writes();
    rig.feed_cc(20, 0);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(before, rig.cc.writes());
    TEST_ASSERT_EQUAL(3, seq->pulses());
}

// ---------------------------------------------------------------------------
// Targets beyond nodes.
// ---------------------------------------------------------------------------
static void test_a_cc_can_drive_the_clock_source_and_ppqn() {
    Rig rig;
    CcMapping m = unused_mapping();
    m.source_mask = KEYBOARD;
    m.channel = 1;
    m.cc = 30;
    m.target_kind = CC_TARGET_CLOCK;
    m.param = CC_CLOCK_SOURCE;
    install(rig, m);

    rig.feed_cc(30, 127);
    rig.pass(1000);
    TEST_ASSERT_EQUAL(MasterClock::CLOCK_MIDI, rig.master.clock().source());
    rig.feed_cc(30, 0);
    rig.pass(2000);
    TEST_ASSERT_EQUAL(MasterClock::CLOCK_INTERNAL, rig.master.clock().source());
}

static void test_transport_targets_fire_once_on_the_way_up() {
    Rig rig;
    CcMapping m = unused_mapping();
    m.source_mask = KEYBOARD;
    m.channel = 1;
    m.cc = 31;
    m.target_kind = CC_TARGET_TRANSPORT;
    m.param = CC_TRANSPORT_STOP;
    install(rig, m);

    TEST_ASSERT_TRUE(rig.master.clock().running());
    rig.feed_cc(31, 127);
    rig.pass(1000);
    TEST_ASSERT_FALSE(rig.master.clock().running());

    // Releasing the button does nothing, and pressing again re-fires.
    rig.master.clock().start();
    rig.feed_cc(31, 0);
    rig.pass(2000);
    TEST_ASSERT_TRUE(rig.master.clock().running());
    rig.feed_cc(31, 127);
    rig.pass(3000);
    TEST_ASSERT_FALSE(rig.master.clock().running());
}

// Tap tempo: MasterClock's header has promised it since #4 and there was no
// entry point until now.
static void test_tap_tempo_sets_the_tempo() {
    Rig rig;
    CcMapping m = unused_mapping();
    m.source_mask = KEYBOARD;
    m.channel = 1;
    m.cc = 32;
    m.target_kind = CC_TARGET_TRANSPORT;
    m.param = CC_TRANSPORT_TAP;
    install(rig, m);

    // Four taps 500 ms apart: 120 BPM.
    uint32_t now = 1000000;
    for (int i = 0; i < 5; i++) {
        rig.feed_cc(32, 127, 1, KEYBOARD, now);
        rig.cc.apply(now);
        rig.feed_cc(32, 0, 1, KEYBOARD, now + 1000);
        rig.cc.apply(now + 1000);
        now += 500000;
    }
    TEST_ASSERT_INT_WITHIN(2, 120, rig.master.clock().bpm());

    // A tap after a long pause starts a new measurement rather than dragging
    // the average down.
    now += 30000000;
    rig.feed_cc(32, 127, 1, KEYBOARD, now);
    rig.cc.apply(now);
    TEST_ASSERT_INT_WITHIN(2, 120, rig.master.clock().bpm());
}

// A tap implying an impossible tempo is ignored, not clamped: a stray double
// hit should not pin the module to 20 BPM.
static void test_an_implausible_tap_is_ignored() {
    MasterClock clock;
    clock.set_bpm(120);
    uint32_t now = 1000000;
    clock.tap(now);
    clock.tap(now + 1000);            // 1 ms apart: 60000 BPM
    TEST_ASSERT_EQUAL(120, clock.bpm());
}

// ---------------------------------------------------------------------------
// Rate limiting: the property that makes a sweep affordable.
// ---------------------------------------------------------------------------
static void test_a_thousand_messages_in_one_pass_produce_one_write() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3));
    const uint32_t before = rig.cc.writes();

    for (int i = 0; i < 1000; i++) rig.feed_cc(20, (uint8_t)(i % 128));
    rig.pass(1000);
    TEST_ASSERT_EQUAL_MESSAGE(before + 1u, rig.cc.writes(),
                              "a sweep must cost one write per mapping per pass");

    // And the value applied is the newest one seen, not the first.
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    const uint16_t expected = (uint16_t)(((uint32_t)(999 % 128) * MAX_SEQUENCE_LEN + 63u) / 127u);
    TEST_ASSERT_EQUAL(expected, seq->pulses());
}

static void test_an_unchanged_value_costs_nothing_downstream() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3));
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    rig.feed_cc(20, 64);
    rig.pass(1000);
    const uint32_t pattern = seq->pattern();

    // Re-sending the same value re-derives nothing: #20's no-op-set rule.
    for (int i = 0; i < 50; i++) { rig.feed_cc(20, 64); rig.pass((uint32_t)(2000 + i * 100)); }
    TEST_ASSERT_EQUAL(pattern, seq->pattern());
}

// ---------------------------------------------------------------------------
// Validation: a patch with a bad mapping is rejected whole.
// ---------------------------------------------------------------------------
static void test_a_patch_with_an_invalid_mapping_is_rejected_whole() {
    Rig rig;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(mappable_patch(), g, 0));

    struct BadCase { CcMapping m; const char* why; };
    CcMapping bad_node = node_mapping(20, 9, 3);              // no such node
    CcMapping bad_param = node_mapping(20, 0, 900);           // no such parameter
    CcMapping bad_range = node_mapping(20, 0, 3, 20, 5);      // min > max
    CcMapping bad_kind = node_mapping(20, 0, 3);
    bad_kind.target_kind = CC_TARGET_PORT;                    // reserved, not built
    CcMapping bad_cc = node_mapping(125, 0, 3);               // a channel mode message
    CcMapping control = node_mapping(20, 0, 3);
    control.source_mask = MIDI_CONTROL_PORT;                  // the protocol's own port

    const BadCase cases[] = {
        {bad_node, "node index"}, {bad_param, "parameter index"},
        {bad_range, "min > max"}, {bad_kind, "reserved target"},
        {bad_cc, "channel mode message"}, {control, "the control cable"},
    };
    for (const BadCase& c : cases) {
        Patch p = mappable_patch();
        p.cc_map[0] = c.m;
        TEST_ASSERT_EQUAL_MESSAGE(APPLY_INVALID, rig.patches.apply(p, g, 1000), c.why);
        // And the running patch is untouched.
        TEST_ASSERT_EQUAL(2, rig.master.node_count());
    }

    // The valid one still loads.
    Patch good = mappable_patch();
    good.cc_map[0] = node_mapping(20, 0, 3);
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(good, g, 2000));
}

// ---------------------------------------------------------------------------
// Mappings travel with the patch.
// ---------------------------------------------------------------------------
static void test_mappings_round_trip_through_the_codec() {
    static uint8_t buffer[PATCH_SLOT_BYTES];
    Patch p = mappable_patch();
    p.cc_map[0] = node_mapping(20, 0, 3, 4, 16, CC_TAKEOVER_SCALE | CC_PASS_THROUGH);
    p.cc_map[7] = unused_mapping();
    p.cc_map[7].source_mask = KEYBOARD;
    p.cc_map[7].cc = 10;
    p.cc_map[7].target_kind = CC_TARGET_CLOCK;
    p.cc_map[7].param = CC_CLOCK_TEMPO;
    p.cc_map[7].min = 60;
    p.cc_map[7].max = 180;
    p.cc_map[7].flags = CC_FOURTEEN_BIT;

    GlobalSettings g = default_globals();
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(p, g, buffer, sizeof buffer, written));

    Patch back = empty_patch();
    GlobalSettings back_globals = {};
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(buffer, written, back, back_globals));

    TEST_ASSERT_EQUAL(20, back.cc_map[0].cc);
    TEST_ASSERT_EQUAL(4, back.cc_map[0].min);
    TEST_ASSERT_EQUAL(16, back.cc_map[0].max);
    TEST_ASSERT_EQUAL(CC_TAKEOVER_SCALE | CC_PASS_THROUGH, back.cc_map[0].flags);
    TEST_ASSERT_EQUAL(CC_TARGET_CLOCK, back.cc_map[7].target_kind);
    TEST_ASSERT_EQUAL(180, back.cc_map[7].max);
    // Untouched slots stay unused, and cost nothing on the wire.
    TEST_ASSERT_EQUAL(0, back.cc_map[3].source_mask);
}

// ---------------------------------------------------------------------------
// Learn, over the control channel because there is no button.
// ---------------------------------------------------------------------------
static void test_learn_binds_the_next_cc_and_reports_it() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(mappable_patch(), g, 0);

    rig.cc.learn_arm(0, CC_TARGET_NODE, 0, 3, 0);
    TEST_ASSERT_TRUE(rig.cc.learning());

    // The reserved control cable is ignored while armed: a learn that bound
    // to its own control port would be a trap.
    rig.feed_cc(70, 100, 1, MIDI_CONTROL_PORT, 1000);
    TEST_ASSERT_TRUE(rig.cc.learning());

    // A CC from the controller binds.
    rig.feed_cc(45, 100, 3, KEYBOARD, 2000);
    TEST_ASSERT_FALSE(rig.cc.learning());
    TEST_ASSERT_EQUAL(45, rig.patches.active().cc_map[0].cc);
    TEST_ASSERT_EQUAL(3, rig.patches.active().cc_map[0].channel);
    TEST_ASSERT_EQUAL(KEYBOARD, rig.patches.active().cc_map[0].source_mask);

    // Reported to the host exactly once.
    uint8_t slot = 0xFF;
    TEST_ASSERT_TRUE(rig.cc.take_learn_result(slot));
    TEST_ASSERT_EQUAL(0, slot);
    TEST_ASSERT_FALSE(rig.cc.take_learn_result(slot));

    // And the binding works.
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));
    rig.feed_cc(45, 127, 3, KEYBOARD, 3000);
    rig.pass(4000);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN, seq->pulses());
}

static void test_learn_times_out() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(mappable_patch(), g, 0);
    rig.cc.learn_arm(0, CC_TARGET_NODE, 0, 3, 0);
    rig.cc.apply(CcMapper::LEARN_TIMEOUT_US + 1u);
    TEST_ASSERT_FALSE(rig.cc.learning());
    TEST_ASSERT_EQUAL(0, rig.patches.active().cc_map[0].source_mask);
}

static void test_learn_can_be_cancelled() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(mappable_patch(), g, 0);
    rig.cc.learn_arm(0, CC_TARGET_NODE, 0, 3, 0);
    rig.cc.learn_cancel();
    TEST_ASSERT_FALSE(rig.cc.learning());
    rig.feed_cc(45, 100, 1, KEYBOARD, 1000);
    TEST_ASSERT_EQUAL(0, rig.patches.active().cc_map[0].source_mask);
}

// ---------------------------------------------------------------------------
// Held notes, and no allocation.
// ---------------------------------------------------------------------------
static void test_a_sweep_under_a_held_chord_hangs_nothing() {
    Rig rig;
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, 0};
    p.nodes[0] = node_config(ALGO_TRANSPOSE);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 1;
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{KEYBOARD, 0, 1};
    p.cc_map[0] = node_mapping(20, 0, 0);            // the transpose offset
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));

    uint32_t now = 0;
    for (uint8_t note : {60, 64, 67}) {
        const MidiEvent e = {MIDI_NOTE_ON, 1, note, 100};
        rig.master.deliver_midi(KEYBOARD, e);
    }
    rig.pass(now); now += 1000;

    // Sweep the offset right across while the chord is held.
    for (int v = 0; v < 128; v += 4) {
        rig.feed_cc(20, (uint8_t)v);
        rig.pass(now); now += 1000;
    }
    for (uint8_t note : {60, 64, 67}) {
        const MidiEvent e = {MIDI_NOTE_OFF, 1, note, 0};
        rig.master.deliver_midi(KEYBOARD, e);
    }
    rig.pass(now);
    rig.master.unload();

    int balance[128] = {0};
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_NOTE_ON && m.d2 > 0) balance[m.d1]++;
        else if (m.type == MIDI_NOTE_OFF || (m.type == MIDI_NOTE_ON && m.d2 == 0)) balance[m.d1]--;
    }
    for (int n = 0; n < 128; n++) {
        TEST_ASSERT_EQUAL_MESSAGE(0, balance[n], "a CC sweep hung a note");
    }
}

static void test_the_mapping_path_never_allocates() {
    Rig rig;
    install(rig, node_mapping(20, 0, 3));
    rig.feed_cc(20, 1);
    rig.pass(0);                                  // warm anything that would

    const size_t before = g_allocations;
    uint32_t now = 1000;
    for (int i = 0; i < 500; i++) {
        rig.cc.observe(KEYBOARD, 1, 20, (uint8_t)(i % 128), now);
        if ((i % 10) == 0) { rig.cc.apply(now); now += 1000; }
    }
    TEST_ASSERT_EQUAL(before, g_allocations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_mapped_cc_moves_its_parameter);
    RUN_TEST(test_the_wrong_channel_port_or_number_changes_nothing);
    RUN_TEST(test_omni_matches_every_channel);
    RUN_TEST(test_a_cc_on_euclid_changes_the_pattern_without_moving_the_cursor);
    RUN_TEST(test_a_sub_range_limits_the_sweep);
    RUN_TEST(test_a_mapped_cc_is_consumed_by_default);
    RUN_TEST(test_a_pass_through_cc_reaches_the_graph_as_well);
    RUN_TEST(test_a_sustain_output_is_not_affected_by_a_mapping);
    RUN_TEST(test_fourteen_bit_tempo_resolves_the_whole_range);
    RUN_TEST(test_a_lone_msb_is_applied_and_does_not_stall);
    RUN_TEST(test_jump_takes_the_value_immediately);
    RUN_TEST(test_pickup_waits_until_the_knob_crosses_the_value);
    RUN_TEST(test_scale_reaches_both_ends_from_wherever_the_knob_is);
    RUN_TEST(test_relative_twos_complement);
    RUN_TEST(test_relative_signed_bit);
    RUN_TEST(test_relative_offset_64);
    RUN_TEST(test_a_relative_zero_writes_nothing);
    RUN_TEST(test_a_cc_can_drive_the_clock_source_and_ppqn);
    RUN_TEST(test_transport_targets_fire_once_on_the_way_up);
    RUN_TEST(test_tap_tempo_sets_the_tempo);
    RUN_TEST(test_an_implausible_tap_is_ignored);
    RUN_TEST(test_a_thousand_messages_in_one_pass_produce_one_write);
    RUN_TEST(test_an_unchanged_value_costs_nothing_downstream);
    RUN_TEST(test_a_patch_with_an_invalid_mapping_is_rejected_whole);
    RUN_TEST(test_mappings_round_trip_through_the_codec);
    RUN_TEST(test_learn_binds_the_next_cc_and_reports_it);
    RUN_TEST(test_learn_times_out);
    RUN_TEST(test_learn_can_be_cancelled);
    RUN_TEST(test_a_sweep_under_a_held_chord_hangs_nothing);
    RUN_TEST(test_the_mapping_path_never_allocates);
    return UNITY_END();
}
