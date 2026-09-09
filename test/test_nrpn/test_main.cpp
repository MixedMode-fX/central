#include <unity.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "control/midi_dispatch.h"
#include "control/cc_mapper.h"
#include "control/nrpn.h"
#include "protocol/sysex_handler.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "patch/default_patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/scale.h"
#include "algorithm/sequencer/note_sequencer.h"
#include "algorithm/sequencer/sequencers.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// What CC cannot reach (#22): NRPN, addressed pattern data, and step-record.

static constexpr uint8_t KEYBOARD = mmMIDI_SERIAL_1;

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
    NrpnDecoder nrpn;
    SysexHandler sysex;

    Rig() : gpio(), midi(), eeprom(), led_driver(),
            master(gpio, midi), leds(led_driver), store(eeprom),
            patches(master, store, leds), cc(patches, master), nrpn(patches, cc),
            sysex(patches, master, store, leds, midi, cc) {}

    // The whole input path, as main.cpp runs it.
    bool feed_cc(uint8_t number, uint8_t value, uint8_t channel = 1,
                 uint8_t source = KEYBOARD, uint32_t now_us = 0) {
        if (nrpn.observe(source, channel, number, value, now_us)) return true;
        if (cc.observe(source, channel, number, value, now_us)) return true;
        const MidiEvent e = {MIDI_CONTROL_CHANGE, channel, number, value};
        master.deliver_midi(source, e, now_us);
        return false;
    }

    void nrpn_write(uint16_t address, uint16_t value, uint8_t channel = 1, uint32_t now_us = 0) {
        feed_cc(99, (uint8_t)((address >> 7) & 0x7F), channel, KEYBOARD, now_us);
        feed_cc(98, (uint8_t)(address & 0x7F), channel, KEYBOARD, now_us);
        feed_cc(6,  (uint8_t)((value >> 7) & 0x7F), channel, KEYBOARD, now_us);
        feed_cc(38, (uint8_t)(value & 0x7F), channel, KEYBOARD, now_us);
        cc.apply(now_us);
    }

    void enable_nrpn(uint8_t channel = 0, uint8_t mask = 0) {
        GlobalSettings g = patches.globals();
        g.nrpn_enabled = 1;
        g.nrpn_channel = channel;
        g.nrpn_source_mask = mask;
        patches.set_globals(g, 0);
    }

    void send(uint8_t command, const std::vector<uint8_t>& args = {}) {
        std::vector<uint8_t> m{0xF0, SYSEX_MANUFACTURER, sysex.device_id(), command,
                               SYSEX_PROTOCOL_VERSION};
        for (uint8_t b : args) m.push_back(b);
        m.push_back(0xF7);
        sysex.deliver_sysex(MIDI_CONTROL_PORT, m.data(), (uint16_t)m.size(), 0);
    }
    bool acked() const { return midi.last_reply(SYSEX_ACK) != nullptr; }
};

static Patch euclid_patch() {
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_EUCLID_SEQ);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 1;
    p.nodes[0].params[0] = 8;
    p.nodes[0].params[3] = 3;
    p.n_nodes = 1;
    return p;
}

// A mono note sequencer with a keyboard on its record inlet.
static Patch record_patch() {
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};      // jack 1: advance
    p.gate_ports[1] = GatePortConfig{GATE_PORT_IN, 2};      // jack 2: record enable
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, 3};            // the keyboard -> note bus 3
    p.nodes[0] = node_config(ALGO_NOTE_SEQ);
    p.nodes[0].in_bus[0] = 0;                               // advance
    p.nodes[0].in_bus[1] = NO_BUS;                          // reset
    p.nodes[0].in_bus[2] = NO_BUS;                          // root
    p.nodes[0].in_bus[3] = 3;                               // record
    p.nodes[0].in_bus[4] = 2;                               // record enable
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[NoteSequencerBase::P_LENGTH] = 4;
    p.nodes[0].params[NoteSequencerBase::P_ROOT] = 60;
    p.nodes[0].params[NoteSequencerBase::P_SCALE_LO] = (uint8_t)(scale_mask(SCALE_MAJOR) & 0xFF);
    p.nodes[0].params[NoteSequencerBase::P_SCALE_HI] = (uint8_t)(scale_mask(SCALE_MAJOR) >> 8);
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{KEYBOARD, 0, 0};
    return p;
}

// ---------------------------------------------------------------------------
// The address space: written down, total, and reversible.
// ---------------------------------------------------------------------------
static void test_the_address_space_covers_every_target_and_reverses() {
    uint8_t kind = 0, index = 0;
    uint16_t param = 0, addr = 0;

    // Every node parameter has an address, and it round-trips.
    for (uint8_t n = 0; n < N_NODE; n++) {
        for (uint16_t p = 0; p < N_PARAM; p += 37) {
            TEST_ASSERT_TRUE(NrpnDecoder::address_of(CC_TARGET_NODE, n, p, addr));
            TEST_ASSERT_TRUE(NrpnDecoder::resolve(addr, kind, index, param));
            TEST_ASSERT_EQUAL(CC_TARGET_NODE, kind);
            TEST_ASSERT_EQUAL(n, index);
            TEST_ASSERT_EQUAL(p, param);
        }
    }
    // The clock and the transport sit above them.
    TEST_ASSERT_TRUE(NrpnDecoder::address_of(CC_TARGET_CLOCK, 0, CC_CLOCK_TEMPO, addr));
    TEST_ASSERT_EQUAL(NRPN_CLOCK_BASE, addr);
    TEST_ASSERT_TRUE(NrpnDecoder::resolve(addr, kind, index, param));
    TEST_ASSERT_EQUAL(CC_TARGET_CLOCK, kind);
    TEST_ASSERT_EQUAL(CC_CLOCK_TEMPO, param);

    TEST_ASSERT_TRUE(NrpnDecoder::address_of(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_TAP, addr));
    TEST_ASSERT_TRUE(NrpnDecoder::resolve(addr, kind, index, param));
    TEST_ASSERT_EQUAL(CC_TARGET_TRANSPORT, kind);
    TEST_ASSERT_EQUAL(CC_TRANSPORT_TAP, param);

    // And a reserved address resolves to nothing rather than somewhere
    // arbitrary.
    TEST_ASSERT_FALSE(NrpnDecoder::resolve(NRPN_RESERVED_BASE, kind, index, param));
    TEST_ASSERT_FALSE(NrpnDecoder::resolve(0x3FFF, kind, index, param));
    TEST_ASSERT_FALSE(NrpnDecoder::address_of(CC_TARGET_PORT, 0, 0, addr));
}

// ---------------------------------------------------------------------------
// A well-formed sequence writes; a partial one does not.
// ---------------------------------------------------------------------------
static void test_a_well_formed_nrpn_writes_the_addressed_parameter() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint16_t addr = 0;
    TEST_ASSERT_TRUE(NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr));
    rig.nrpn_write(addr, 7);
    TEST_ASSERT_EQUAL(7, seq->pulses());

    rig.nrpn_write(addr, 12);
    TEST_ASSERT_EQUAL(12, seq->pulses());
}

static void test_a_truncated_nrpn_writes_nothing() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);

    // An address with no data.
    rig.feed_cc(99, (uint8_t)(addr >> 7));
    rig.feed_cc(98, (uint8_t)(addr & 0x7F));
    TEST_ASSERT_EQUAL(3, seq->pulses());

    // Data with no address, from cold.
    Rig other;
    other.patches.apply(euclid_patch(), g, 0);
    other.enable_nrpn();
    EuclidianSequencer* s2 = static_cast<EuclidianSequencer*>(other.master.node(0));
    other.feed_cc(6, 9);
    other.feed_cc(38, 0);
    TEST_ASSERT_EQUAL(3, s2->pulses());
    TEST_ASSERT_TRUE(other.nrpn.refused() > 0);
}

// The four CCs can be interleaved with other traffic and still land.
static void test_an_interleaved_nrpn_still_writes_correctly() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);

    rig.feed_cc(99, (uint8_t)(addr >> 7));
    rig.feed_cc(74, 100);                       // an unrelated CC in the middle
    rig.feed_cc(98, (uint8_t)(addr & 0x7F));
    rig.feed_cc(1, 64);                         // and another
    rig.feed_cc(6, 0);
    rig.feed_cc(38, 11);
    rig.cc.apply(0);
    TEST_ASSERT_EQUAL(11, seq->pulses());
}

// A sequence that stops halfway must not pair its address with the next
// gesture's value.
static void test_a_stale_sequence_times_out() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);
    rig.feed_cc(99, (uint8_t)(addr >> 7), 1, KEYBOARD, 1000);
    rig.feed_cc(98, (uint8_t)(addr & 0x7F), 1, KEYBOARD, 1000);
    TEST_ASSERT_TRUE(rig.nrpn.in_sequence());

    rig.nrpn.service(1000 + NRPN_TIMEOUT_US + 1u);
    TEST_ASSERT_FALSE(rig.nrpn.in_sequence());

    // The value that arrives now is orphaned, and writes nothing.
    rig.feed_cc(6, 0, 1, KEYBOARD, 2000000000u);
    rig.feed_cc(38, 20, 1, KEYBOARD, 2000000000u);
    TEST_ASSERT_EQUAL(3, seq->pulses());
}

// NRPN and CC writing the same parameter agree, and are rejected identically.
static void test_nrpn_and_cc_agree_on_the_same_parameter() {
    Rig via_cc;
    Rig via_nrpn;
    GlobalSettings g = default_globals();

    Patch p = euclid_patch();
    p.cc_map[0] = unused_mapping();
    p.cc_map[0].source_mask = KEYBOARD;
    p.cc_map[0].channel = 1;
    p.cc_map[0].cc = 20;
    p.cc_map[0].target_kind = CC_TARGET_NODE;
    p.cc_map[0].target_index = 0;
    p.cc_map[0].param = 3;
    p.cc_map[0].min = 9;
    p.cc_map[0].max = 9;                         // pinned, so both write 9
    via_cc.patches.apply(p, g, 0);
    via_nrpn.patches.apply(euclid_patch(), g, 0);
    via_nrpn.enable_nrpn();

    via_cc.feed_cc(20, 64);
    via_cc.cc.apply(1000);

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);
    via_nrpn.nrpn_write(addr, 9);

    TEST_ASSERT_EQUAL(static_cast<EuclidianSequencer*>(via_cc.master.node(0))->pulses(),
                      static_cast<EuclidianSequencer*>(via_nrpn.master.node(0))->pulses());

    // Both clamp into the target's range rather than writing something the
    // validator would refuse.
    via_nrpn.nrpn_write(addr, 16383);
    TEST_ASSERT_EQUAL(MAX_SEQUENCE_LEN,
                      static_cast<EuclidianSequencer*>(via_nrpn.master.node(0))->pulses());
}

// ---------------------------------------------------------------------------
// NRPN is a routable CC stream, so it is off by default.
// ---------------------------------------------------------------------------
static void test_nrpn_traffic_passes_through_where_it_is_not_enabled() {
    Rig rig;
    Patch p = euclid_patch();
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, 0};
    p.midi_out[0] = MidiOutConfig{KEYBOARD, 0, 0};
    GlobalSettings g = default_globals();
    rig.patches.apply(p, g, 0);
    TEST_ASSERT_EQUAL(0, rig.patches.globals().nrpn_enabled);

    // Every NRPN controller passes through untouched.
    for (uint8_t number : {99, 98, 6, 38, 96, 97}) {
        TEST_ASSERT_FALSE_MESSAGE(rig.feed_cc(number, 42), "NRPN must be off by default");
    }
    uint32_t now = 0;
    for (int i = 0; i < 3; i++) { rig.master.pass(now); now += 1000; }

    uint8_t seen = 0;
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_CONTROL_CHANGE) seen++;
    }
    TEST_ASSERT_EQUAL_MESSAGE(6, seen, "every NRPN byte should have reached the note bus");
    // And the parameter it would have addressed is untouched.
    TEST_ASSERT_EQUAL(3, static_cast<EuclidianSequencer*>(rig.master.node(0))->pulses());
}

static void test_nrpn_can_be_limited_to_one_port_and_channel() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn(5, KEYBOARD);
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);

    rig.nrpn_write(addr, 9, 2);                  // wrong channel
    TEST_ASSERT_EQUAL(3, seq->pulses());

    rig.feed_cc(99, (uint8_t)(addr >> 7), 5, mmMIDI_SERIAL_2);   // wrong port
    rig.feed_cc(98, (uint8_t)(addr & 0x7F), 5, mmMIDI_SERIAL_2);
    rig.feed_cc(6, 0, 5, mmMIDI_SERIAL_2);
    TEST_ASSERT_EQUAL(3, seq->pulses());

    rig.nrpn_write(addr, 9, 5);                  // and now the right one
    TEST_ASSERT_EQUAL(9, seq->pulses());
}

// Data increment and decrement, which is how a controller nudges a parameter
// without knowing its current value.
static void test_data_increment_and_decrement() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();
    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(rig.master.node(0));

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);
    rig.feed_cc(99, (uint8_t)(addr >> 7));
    rig.feed_cc(98, (uint8_t)(addr & 0x7F));

    rig.feed_cc(96, 1);
    TEST_ASSERT_EQUAL(4, seq->pulses());
    rig.feed_cc(96, 3);
    TEST_ASSERT_EQUAL(7, seq->pulses());
    rig.feed_cc(97, 2);
    TEST_ASSERT_EQUAL(5, seq->pulses());

    // It clamps at the bottom rather than wrapping.
    for (int i = 0; i < 20; i++) rig.feed_cc(97, 1);
    TEST_ASSERT_EQUAL(0, seq->pulses());
}

static void test_nrpn_can_drive_the_clock() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();

    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_CLOCK, 0, CC_CLOCK_TEMPO, addr);
    rig.nrpn_write(addr, 175);
    TEST_ASSERT_EQUAL(175, rig.master.clock().bpm());
}

// ---------------------------------------------------------------------------
// Pattern data over SysEx.
// ---------------------------------------------------------------------------
static void test_pattern_data_round_trips_over_sysex() {
    Rig rig;
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_POLY_SEQ);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[NoteSequencerBase::P_LENGTH] = 32;
    p.n_nodes = 1;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));

    // A full 32-step grid, written in runs.
    const uint16_t base = NoteSequencerBase::STEP_BASE;
    const uint16_t total = (uint16_t)(NoteSequencerBase::param_count(NOTE_SEQ_VOICES) - base);
    std::vector<uint8_t> pattern;
    for (uint16_t i = 0; i < total; i++) pattern.push_back((uint8_t)((i * 7u) % 100u));

    for (uint16_t at = 0; at < total; at += 64) {
        const uint16_t n = (uint16_t)((uint16_t)(total - at) < (uint16_t)64 ? (uint16_t)(total - at) : (uint16_t)64);
        uint8_t packed[128];
        const size_t packed_len = sysex::pack(&pattern[at], n, packed, sizeof packed);
        std::vector<uint8_t> args{0, (uint8_t)((base + at) & 0x7F), (uint8_t)((base + at) >> 7)};
        for (size_t i = 0; i < packed_len; i++) args.push_back(packed[i]);
        rig.midi.clear();
        rig.send(SYSEX_SET_PATTERN, args);
        TEST_ASSERT_TRUE_MESSAGE(rig.acked(), "a pattern run should be accepted");
    }

    // And it reads back byte-identically.
    std::vector<uint8_t> back;
    for (uint16_t at = 0; at < total; at = (uint16_t)(at + SYSEX_CHUNK_PAYLOAD)) {
        const uint16_t n = (uint16_t)((uint16_t)(total - at) < (uint16_t)SYSEX_CHUNK_PAYLOAD
                                      ? (uint16_t)(total - at) : (uint16_t)SYSEX_CHUNK_PAYLOAD);
        rig.midi.clear();
        rig.send(SYSEX_GET_PATTERN, {0, (uint8_t)((base + at) & 0x7F), (uint8_t)((base + at) >> 7),
                                     (uint8_t)n});
        const auto* r = rig.midi.last_reply(SYSEX_PATTERN);
        TEST_ASSERT_NOT_NULL(r);
        const size_t packed_at = 9;
        const size_t packed_len = r->bytes.size() - packed_at - 1u;
        uint8_t out[SYSEX_CHUNK_PAYLOAD];
        const size_t got = sysex::unpack(&r->bytes[packed_at], packed_len, out, sizeof out);
        for (size_t i = 0; i < got && back.size() < (size_t)total; i++) back.push_back(out[i]);
    }
    TEST_ASSERT_EQUAL(total, back.size());
    for (uint16_t i = 0; i < total; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(pattern[i], back[i], "a pattern byte did not round-trip");
    }
}

// A patch carrying a full note sequence round-trips through the store, which
// is what "the preset format can hold a pattern" means.
static void test_a_full_note_sequence_survives_the_store() {
    Rig rig;
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_POLY_SEQ);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 0;
    for (uint8_t step = 0; step < MAX_SEQUENCE_LEN; step++) {
        uint8_t* b = &p.nodes[0].params[NoteSequencerBase::STEP_BASE
                                       + step * NoteSequencerBase::stride(NOTE_SEQ_VOICES)];
        for (uint8_t v = 0; v < NOTE_SEQ_VOICES; v++) {
            b[v * 2u] = (uint8_t)(step + v);       // degree
            b[v * 2u + 1u] = (uint8_t)(60 + v);    // velocity
        }
        b[NOTE_SEQ_VOICES * 2u] = 2;               // two edges long
        b[NOTE_SEQ_VOICES * 2u + 1u] = 80;         // probability
    }
    p.n_nodes = 1;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, g, 0));
    TEST_ASSERT_EQUAL_MESSAGE(APPLY_OK, rig.patches.save_slot(1),
                              "a full poly grid has to fit a preset slot");

    Patch back;
    GlobalSettings back_globals;
    TEST_ASSERT_EQUAL(STORE_OK, rig.store.load(1, back, back_globals));
    for (uint16_t i = 0; i < NoteSequencerBase::param_count(NOTE_SEQ_VOICES); i++) {
        TEST_ASSERT_EQUAL(p.nodes[0].params[i], back.nodes[0].params[i]);
    }
    // The size the budget has to hold, reported rather than assumed.
    TEST_ASSERT_TRUE(rig.store.used(1) > 0);
    TEST_ASSERT_TRUE_MESSAGE(rig.store.used(1) < PATCH_SLOT_BYTES,
                             "a full poly sequence must fit a slot with room to spare");
}

static void test_a_pattern_write_to_a_missing_node_is_refused() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_SET_PATTERN, {9, 0, 0, 0, 1});
    const auto* nak = rig.midi.last_reply(SYSEX_NAK);
    TEST_ASSERT_NOT_NULL(nak);
    rig.midi.clear();
    rig.send(SYSEX_GET_PATTERN, {9, 0, 0, 8});
    TEST_ASSERT_NOT_NULL(rig.midi.last_reply(SYSEX_NAK));
}

// Any target, by kind, read without a full dump.
static void test_a_control_read_reports_the_value_and_its_nrpn_address() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);

    rig.send(SYSEX_GET_CONTROL, {CC_TARGET_NODE, 0, 3, 0});
    const auto* r = rig.midi.last_reply(SYSEX_CONTROL_VALUE);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(CC_TARGET_NODE, r->bytes[5]);
    TEST_ASSERT_EQUAL(3, (uint16_t)(r->bytes[9] | (r->bytes[10] << 7)));   // the value

    rig.midi.clear();
    rig.send(SYSEX_GET_CONTROL, {CC_TARGET_CLOCK, 0, CC_CLOCK_TEMPO, 0});
    const auto* c = rig.midi.last_reply(SYSEX_CONTROL_VALUE);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(CLOCK_DEFAULT_BPM, (uint16_t)(c->bytes[9] | (c->bytes[10] << 7)));
    TEST_ASSERT_EQUAL(NRPN_CLOCK_BASE, (uint16_t)(c->bytes[11] | (c->bytes[12] << 7)));
}

// ---------------------------------------------------------------------------
// Step-record.
// ---------------------------------------------------------------------------

// The scale conversion, on its own: degree -> semitone -> degree is identity.
static void test_pitch_and_degree_round_trip_through_every_scale() {
    for (uint8_t id = 0; id < SCALE_COUNT; id++) {
        const uint16_t mask = scale_mask(id);
        for (int16_t degree = -20; degree <= 20; degree++) {
            const int16_t semitones = scale_degree_to_semitone(degree, mask);
            char msg[64];
            snprintf(msg, sizeof msg, "scale %u degree %d", id, degree);
            TEST_ASSERT_EQUAL_MESSAGE(degree, semitone_to_scale_degree(semitones, mask), msg);
        }
    }
}

static void play(Rig& rig, uint8_t note, uint32_t& now) {
    const MidiEvent on = {MIDI_NOTE_ON, 1, note, 100};
    rig.master.deliver_midi(KEYBOARD, on);
    rig.master.pass(now); now += 1000;
    rig.master.pass(now); now += 1000;
    const MidiEvent off = {MIDI_NOTE_OFF, 1, note, 0};
    rig.master.deliver_midi(KEYBOARD, off);
    rig.master.pass(now); now += 1000;
    rig.master.pass(now); now += 1000;
}

static void test_step_record_writes_degrees_against_the_root_and_scale() {
    Rig rig;
    GlobalSettings g = default_globals();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(record_patch(), g, 0));
    NoteSequencer* seq = static_cast<NoteSequencer*>(rig.master.node(0));
    TEST_ASSERT_TRUE(seq->recording());

    uint32_t now = 0;
    rig.gpio.set_input(1, true);                  // record enable high
    rig.master.pass(now); now += 1000;

    // C major, root 60: C D E G is degrees 0 1 2 4.
    play(rig, 60, now);
    play(rig, 62, now);
    play(rig, 64, now);
    play(rig, 67, now);

    TEST_ASSERT_EQUAL(0, seq->degree(0, 0));
    TEST_ASSERT_EQUAL(1, seq->degree(1, 0));
    TEST_ASSERT_EQUAL(2, seq->degree(2, 0));
    TEST_ASSERT_EQUAL(4, seq->degree(3, 0));
    TEST_ASSERT_EQUAL(0, seq->record_cursor());   // wrapped at the length

    // And playing it back sends the pitches that were played.
    std::vector<uint8_t> played;
    for (int i = 0; i < 4; i++) {
        rig.gpio.set_input(0, true);  rig.master.pass(now); now += 1000;
        rig.gpio.set_input(0, false); rig.master.pass(now); now += 1000;
    }
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_NOTE_ON && m.d2 > 0) played.push_back(m.d1);
    }
    TEST_ASSERT_EQUAL(4, played.size());
    TEST_ASSERT_EQUAL(60, played[0]);
    TEST_ASSERT_EQUAL(62, played[1]);
    TEST_ASSERT_EQUAL(64, played[2]);
    TEST_ASSERT_EQUAL(67, played[3]);
}

// The documented rule for a note the scale does not contain: it snaps to the
// nearest tone in it, and the snap is counted so a user can see it happened.
static void test_an_out_of_scale_note_snaps_and_is_counted() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(record_patch(), g, 0);
    NoteSequencer* seq = static_cast<NoteSequencer*>(rig.master.node(0));

    uint32_t now = 0;
    rig.gpio.set_input(1, true);
    rig.master.pass(now); now += 1000;

    TEST_ASSERT_EQUAL(0, seq->snapped());
    play(rig, 61, now);                            // C# is not in C major
    TEST_ASSERT_EQUAL_MESSAGE(1, seq->snapped(), "an out-of-scale note must be counted");
    // 61 snaps up to 62 (a tie goes up, which keeps a rising line rising),
    // which is degree 1.
    TEST_ASSERT_EQUAL(1, seq->degree(0, 0));
}

static void test_record_enable_gates_recording() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(record_patch(), g, 0);
    NoteSequencer* seq = static_cast<NoteSequencer*>(rig.master.node(0));

    uint32_t now = 0;
    rig.gpio.set_input(1, false);                  // record enable low
    rig.master.pass(now); now += 1000;
    play(rig, 67, now);
    TEST_ASSERT_EQUAL(0, seq->record_cursor());
    TEST_ASSERT_EQUAL(0, seq->velocity(0, 0));     // nothing written

    rig.gpio.set_input(1, true);
    rig.master.pass(now); now += 1000;
    play(rig, 67, now);
    TEST_ASSERT_EQUAL(1, seq->record_cursor());
    TEST_ASSERT_EQUAL(4, seq->degree(0, 0));
}

static void test_the_reserved_keys_write_a_rest_and_a_tie() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(record_patch(), g, 0);
    NoteSequencer* seq = static_cast<NoteSequencer*>(rig.master.node(0));

    uint32_t now = 0;
    rig.gpio.set_input(1, true);
    rig.master.pass(now); now += 1000;

    play(rig, 60, now);                            // step 0: a note
    play(rig, NoteSequencerBase::DEFAULT_REST_KEY, now);   // step 1: a rest
    play(rig, NoteSequencerBase::DEFAULT_TIE_KEY, now);    // step 2: a tie
    play(rig, 64, now);                            // step 3: a note

    TEST_ASSERT_EQUAL(0, seq->flags(0) & (NoteSequencerBase::FLAG_REST | NoteSequencerBase::FLAG_TIE));
    TEST_ASSERT_TRUE(seq->flags(1) & NoteSequencerBase::FLAG_REST);
    TEST_ASSERT_TRUE(seq->flags(2) & NoteSequencerBase::FLAG_TIE);
    TEST_ASSERT_EQUAL(2, seq->degree(3, 0));
    TEST_ASSERT_EQUAL(0, seq->record_cursor());
}

static void test_reset_returns_the_record_cursor_to_the_first_step() {
    Rig rig;
    Patch p = record_patch();
    p.gate_ports[2] = GatePortConfig{GATE_PORT_IN, 4};
    p.nodes[0].in_bus[1] = 4;                       // reset from jack 3
    GlobalSettings g = default_globals();
    rig.patches.apply(p, g, 0);
    NoteSequencer* seq = static_cast<NoteSequencer*>(rig.master.node(0));

    uint32_t now = 0;
    rig.gpio.set_input(1, true);
    rig.master.pass(now); now += 1000;
    play(rig, 60, now);
    play(rig, 62, now);
    TEST_ASSERT_EQUAL(2, seq->record_cursor());

    rig.gpio.set_input(2, true);  rig.master.pass(now); now += 1000;
    rig.gpio.set_input(2, false); rig.master.pass(now); now += 1000;
    TEST_ASSERT_EQUAL(0, seq->record_cursor());
}

// Recording over a step while the sequencer is playing releases nothing it
// should not: the ledger owns the note-off, so a pattern write cannot strand
// a sounding note.
static void test_recording_during_playback_hangs_nothing() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(record_patch(), g, 0);

    uint32_t now = 0;
    rig.gpio.set_input(1, true);
    rig.master.pass(now); now += 1000;
    play(rig, 60, now);
    play(rig, 64, now);
    play(rig, 67, now);
    play(rig, 71, now);

    // Play it while recording new notes over it.
    for (int i = 0; i < 12; i++) {
        rig.gpio.set_input(0, true);  rig.master.pass(now); now += 1000;
        rig.gpio.set_input(0, false); rig.master.pass(now); now += 1000;
        if ((i % 3) == 0) play(rig, (uint8_t)(60 + i), now);
    }
    rig.master.unload();

    int balance[128] = {0};
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_NOTE_ON && m.d2 > 0) balance[m.d1]++;
        else if (m.type == MIDI_NOTE_OFF || (m.type == MIDI_NOTE_ON && m.d2 == 0)) balance[m.d1]--;
    }
    for (int n = 0; n < 128; n++) {
        TEST_ASSERT_EQUAL_MESSAGE(0, balance[n], "recording during playback hung a note");
    }
}

static void test_the_nrpn_path_never_allocates() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(euclid_patch(), g, 0);
    rig.enable_nrpn();
    uint16_t addr = 0;
    NrpnDecoder::address_of(CC_TARGET_NODE, 0, 3, addr);
    rig.nrpn_write(addr, 5);                        // warm anything that would

    const size_t before = g_allocations;
    for (int i = 0; i < 200; i++) {
        rig.nrpn.observe(KEYBOARD, 1, 99, (uint8_t)(addr >> 7), (uint32_t)i);
        rig.nrpn.observe(KEYBOARD, 1, 98, (uint8_t)(addr & 0x7F), (uint32_t)i);
        rig.nrpn.observe(KEYBOARD, 1, 6, 0, (uint32_t)i);
        rig.nrpn.observe(KEYBOARD, 1, 38, (uint8_t)(i % 32), (uint32_t)i);
        rig.nrpn.service((uint32_t)i);
    }
    TEST_ASSERT_EQUAL(before, g_allocations);
}


// ---------------------------------------------------------------------------
// The input path as main.cpp runs it (control/midi_dispatch.h): the order of
// the control plane, and the drain stopping in front of a full bus.
// ---------------------------------------------------------------------------
static Patch thru_patch() {
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{KEYBOARD, 0, 0};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, 0};
    return p;
}

// A sustain pedal released under a held chord: more note-offs in one burst
// than a bus holds in one pass. Every one of them must reach the output.
static void test_a_note_off_burst_larger_than_the_bus_is_not_dropped() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(thru_patch(), g, 0);
    MidiInputQueue queue;
    const uint8_t burst = NOTE_QUEUE_DEPTH + 4;
    for (uint8_t i = 0; i < burst; i++) queue.push(KEYBOARD, MidiEvent{MIDI_NOTE_OFF, 1, (uint8_t)(40 + i), 0});

    TEST_ASSERT_EQUAL(NOTE_QUEUE_DEPTH, dispatch_midi(queue, rig.sysex, rig.nrpn, rig.cc, rig.master, 0));
    TEST_ASSERT_EQUAL(4, queue.count());
    rig.master.pass(0);
    TEST_ASSERT_EQUAL(NOTE_QUEUE_DEPTH, rig.midi.messages.size());

    TEST_ASSERT_EQUAL(4, dispatch_midi(queue, rig.sysex, rig.nrpn, rig.cc, rig.master, 1000));
    rig.master.pass(1000);
    TEST_ASSERT_EQUAL(burst, rig.midi.messages.size());
    TEST_ASSERT_EQUAL(0, rig.master.buses().note_overflows(0));
    // In order, so a note-on and its note-off cannot swap places.
    for (uint8_t i = 0; i < burst; i++) TEST_ASSERT_EQUAL(40 + i, rig.midi.messages[i].d1);
}

// Realtime bytes reach the clock even while a bus is full: they need no room.
static void test_realtime_is_never_held_behind_a_full_bus() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(thru_patch(), g, 0);
    rig.master.clock().stop();
    MidiInputQueue queue;
    for (uint8_t i = 0; i < NOTE_QUEUE_DEPTH; i++) queue.push(KEYBOARD, MidiEvent{MIDI_NOTE_ON, 1, (uint8_t)(40 + i), 100});
    queue.push(KEYBOARD, MidiEvent{MIDI_START, 0, 0, 0});
    queue.push(KEYBOARD, MidiEvent{MIDI_NOTE_ON, 1, 100, 100});
    dispatch_midi(queue, rig.sysex, rig.nrpn, rig.cc, rig.master, 0);
    // The bus is full; the note behind the start waits, the start went through.
    TEST_ASSERT_EQUAL(1, queue.count());
    TEST_ASSERT_TRUE(rig.master.clock().running());
}

// A mapped CC is consumed before the graph sees it; an unmapped one passes.
static void test_the_dispatcher_runs_the_control_plane_before_the_graph() {
    Rig rig;
    GlobalSettings g = default_globals();
    Patch p = thru_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_bus[0] = 0;
    p.n_nodes = 1;
    p.cc_map[0] = unused_mapping();
    p.cc_map[0].source_mask = KEYBOARD;
    p.cc_map[0].cc = 20;
    p.cc_map[0].target_kind = CC_TARGET_NODE;
    p.cc_map[0].target_index = 0;
    p.cc_map[0].param = 1;
    rig.patches.apply(p, g, 0);

    MidiInputQueue queue;
    queue.push(KEYBOARD, MidiEvent{MIDI_CONTROL_CHANGE, 1, 20, 127});   // mapped: consumed
    queue.push(KEYBOARD, MidiEvent{MIDI_CONTROL_CHANGE, 1, 21, 127});   // unmapped: through
    TEST_ASSERT_EQUAL(2, dispatch_midi(queue, rig.sysex, rig.nrpn, rig.cc, rig.master, 0));
    rig.cc.apply(0);
    rig.master.pass(0);
    TEST_ASSERT_EQUAL(1, rig.midi.messages.size());
    TEST_ASSERT_EQUAL(21, rig.midi.messages[0].d1);
    uint8_t amount = 0;
    TEST_ASSERT_TRUE(rig.master.get_node_param(0, 1, amount));
    TEST_ASSERT_EQUAL(255, amount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_address_space_covers_every_target_and_reverses);
    RUN_TEST(test_a_well_formed_nrpn_writes_the_addressed_parameter);
    RUN_TEST(test_a_truncated_nrpn_writes_nothing);
    RUN_TEST(test_an_interleaved_nrpn_still_writes_correctly);
    RUN_TEST(test_a_stale_sequence_times_out);
    RUN_TEST(test_nrpn_and_cc_agree_on_the_same_parameter);
    RUN_TEST(test_nrpn_traffic_passes_through_where_it_is_not_enabled);
    RUN_TEST(test_nrpn_can_be_limited_to_one_port_and_channel);
    RUN_TEST(test_data_increment_and_decrement);
    RUN_TEST(test_nrpn_can_drive_the_clock);
    RUN_TEST(test_pattern_data_round_trips_over_sysex);
    RUN_TEST(test_a_full_note_sequence_survives_the_store);
    RUN_TEST(test_a_pattern_write_to_a_missing_node_is_refused);
    RUN_TEST(test_a_control_read_reports_the_value_and_its_nrpn_address);
    RUN_TEST(test_pitch_and_degree_round_trip_through_every_scale);
    RUN_TEST(test_step_record_writes_degrees_against_the_root_and_scale);
    RUN_TEST(test_an_out_of_scale_note_snaps_and_is_counted);
    RUN_TEST(test_record_enable_gates_recording);
    RUN_TEST(test_the_reserved_keys_write_a_rest_and_a_tie);
    RUN_TEST(test_reset_returns_the_record_cursor_to_the_first_step);
    RUN_TEST(test_recording_during_playback_hangs_nothing);
    RUN_TEST(test_the_nrpn_path_never_allocates);
    RUN_TEST(test_a_note_off_burst_larger_than_the_bus_is_not_dropped);
    RUN_TEST(test_realtime_is_never_held_behind_a_full_bus);
    RUN_TEST(test_the_dispatcher_runs_the_control_plane_before_the_graph);
    return UNITY_END();
}
