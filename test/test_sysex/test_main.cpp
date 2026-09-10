#include <unity.h>
#include <stdlib.h>
#include <vector>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "protocol/sysex.h"
#include "protocol/sysex_handler.h"
#include "patch/patch_manager.h"
#include "control/cc_mapper.h"
#include "patch/default_patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "midi/global_scale.h"
#include "algorithm/sequencer/gate_sequencer.h"
#include "algorithm/sequencer/sequencers.h"
#include "algorithm/sequencer/drum_sequencer.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// The patch protocol (#11), driven the way an editor drives it: bytes in,
// bytes out, and everything through the same staging -> validate -> swap path
// the console and Program Change recall use.

static constexpr uint8_t CONTROL = MIDI_CONTROL_PORT;

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
    // The time every message is delivered at. Zero by default; a test that
    // cares about uptime moves it, the way a module that has been on for a
    // while has.
    uint32_t now;

    Rig() : gpio(), midi(), eeprom(), led_driver(),
            master(gpio, midi), leds(led_driver), store(eeprom),
            patches(master, store, leds), cc(patches, master),
            sysex(patches, master, store, leds, midi, cc), now(0) {}

    // One command, framed the way the wire carries it.
    void send(uint8_t command, const std::vector<uint8_t>& args = {}) {
        std::vector<uint8_t> m;
        m.push_back(0xF0);
        m.push_back(SYSEX_MANUFACTURER);
        m.push_back(sysex.device_id());
        m.push_back(command);
        m.push_back(SYSEX_PROTOCOL_VERSION);
        for (uint8_t b : args) m.push_back(b);
        m.push_back(0xF7);
        sysex.deliver_sysex(CONTROL, m.data(), (uint16_t)m.size(), now);
    }

    bool acked() const { return midi.last_reply(SYSEX_ACK) != nullptr; }
    bool naked_with(SysexError code) const {
        const RecordingMidiOut::Sysex* s = midi.last_reply(SYSEX_NAK);
        return s != nullptr && s->bytes.size() > 5 && s->bytes[5] == code;
    }
    // Any NAK carrying `code`. A failure can produce several - a bad chunk
    // aborts the transfer, so the chunks behind it are refused too - and
    // which one is last is not the point.
    bool ever_naked_with(SysexError code) const {
        for (const auto& s : midi.sysex) {
            if (s.bytes.size() > 5 && s.bytes[3] == SYSEX_NAK && s.bytes[5] == code) return true;
        }
        return false;
    }
};

static Patch two_node_patch() {
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{0x01, 0, 0};
    p.nodes[0] = node_config(ALGO_TRANSPOSE);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 1;
    p.nodes[0].params[0] = 12;
    p.nodes[1] = node_config(ALGO_STEP_SEQ);
    p.nodes[1].in_bus[0] = 0;             // advance, gate bus 0
    p.nodes[1].out_bus[0] = 2;
    p.nodes[1].params[0] = 8;
    p.nodes[1].params[3] = 0xFF;
    p.n_nodes = 2;
    p.midi_out[0] = MidiOutConfig{0x01, 0, 1};
    return p;
}

static bool patches_equal(const Patch& a, const Patch& b) {
    if (a.n_nodes != b.n_nodes) return false;
    for (uint8_t n = 0; n < a.n_nodes; n++) {
        if (a.nodes[n].algorithm_id != b.nodes[n].algorithm_id) return false;
        for (uint8_t i = 0; i < MAX_IN; i++) if (a.nodes[n].in_bus[i] != b.nodes[n].in_bus[i]) return false;
        for (uint8_t i = 0; i < MAX_OUT; i++) if (a.nodes[n].out_bus[i] != b.nodes[n].out_bus[i]) return false;
        for (uint16_t i = 0; i < N_PARAM; i++) if (a.nodes[n].params[i] != b.nodes[n].params[i]) return false;
    }
    for (uint8_t i = 0; i < GPIO_N; i++) {
        if (a.gate_ports[i].direction != b.gate_ports[i].direction) return false;
        if (a.gate_ports[i].bus != b.gate_ports[i].bus) return false;
    }
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++) {
        if (a.midi_in[i].source_mask != b.midi_in[i].source_mask) return false;
        if (a.midi_in[i].bus != b.midi_in[i].bus) return false;
    }
    return true;
}

// Collects the chunks of a dump and reassembles the image, exactly as an
// editor would have to.
static std::vector<uint8_t> reassemble(const RecordingMidiOut& midi) {
    std::vector<uint8_t> image;
    for (const auto& s : midi.sysex) {
        if (s.bytes.size() < 8 || s.bytes[3] != SYSEX_PATCH_CHUNK_OUT) continue;
        const uint8_t checksum = s.bytes[7];
        const size_t packed_at = 8;
        const size_t packed_len = s.bytes.size() - packed_at - 1u;    // minus F7
        TEST_ASSERT_EQUAL(checksum, sysex::checksum(&s.bytes[packed_at], packed_len));
        uint8_t out[512];
        const size_t n = sysex::unpack(&s.bytes[packed_at], packed_len, out, sizeof out);
        for (size_t i = 0; i < n; i++) image.push_back(out[i]);
    }
    return image;
}

// Splits an image into the chunk messages a host would send.
// `loop_between_chunks` runs the handler's service() between chunks, which is
// what happens on hardware while the host waits for each chunk's ACK: the
// main loop keeps going, and a transfer has to survive it.
static void send_image(Rig& rig, const std::vector<uint8_t>& image, bool corrupt_checksum = false,
                       bool skip_a_chunk = false, bool loop_between_chunks = false) {
    size_t at = 0;
    uint8_t seq = 0;
    bool skipped = false;
    while (at < image.size()) {
        size_t n = image.size() - at;
        if (n > SYSEX_CHUNK_PAYLOAD) n = SYSEX_CHUNK_PAYLOAD;

        uint8_t packed[256];
        const size_t packed_len = sysex::pack(&image[at], n, packed, sizeof packed);
        TEST_ASSERT_TRUE(packed_len > 0);

        uint8_t flags = 0;
        if (at == 0) flags |= SYSEX_CHUNK_FIRST;
        if (at + n >= image.size()) flags |= SYSEX_CHUNK_LAST;

        at += n;
        const uint8_t this_seq = seq;
        seq = (uint8_t)((seq + 1u) & 0x7F);

        if (skip_a_chunk && (flags & SYSEX_CHUNK_FIRST) == 0 && !skipped) {
            skipped = true;
            continue;                     // a chunk lost in flight
        }

        std::vector<uint8_t> args;
        args.push_back(this_seq);
        args.push_back(flags);
        uint8_t sum = sysex::checksum(packed, packed_len);
        if (corrupt_checksum) sum = (uint8_t)((sum + 1u) & 0x7F);
        args.push_back(sum);
        for (size_t i = 0; i < packed_len; i++) args.push_back(packed[i]);
        rig.send(SYSEX_PATCH_CHUNK_IN, args);
        if (loop_between_chunks) { rig.now += 1000; rig.sysex.service(rig.now); }
    }
}

// ---------------------------------------------------------------------------
// 7-bit packing: every byte on the wire is <= 0x7F, and nothing is lost.
// ---------------------------------------------------------------------------
static void test_seven_bit_packing_round_trips_every_byte_value() {
    uint8_t original[256];
    for (int i = 0; i < 256; i++) original[i] = (uint8_t)i;

    uint8_t packed[512];
    const size_t n = sysex::pack(original, sizeof original, packed, sizeof packed);
    TEST_ASSERT_EQUAL(sysex::packed_size(sizeof original), n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(packed[i] <= 0x7F, "a SysEx data byte cannot have bit 7 set");
    }

    uint8_t back[256];
    TEST_ASSERT_EQUAL(sizeof original, sysex::unpack(packed, n, back, sizeof back));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original, back, sizeof original);
}

static void test_packing_refuses_a_buffer_that_is_too_small() {
    uint8_t data[64] = {0};
    uint8_t out[8] = {0};
    TEST_ASSERT_EQUAL(0, sysex::pack(data, sizeof data, out, sizeof out));
    uint8_t packed[128];
    const size_t n = sysex::pack(data, sizeof data, packed, sizeof packed);
    TEST_ASSERT_EQUAL(0, sysex::unpack(packed, n, out, sizeof out));
}

// ---------------------------------------------------------------------------
// Discovery: an editor finds the module and learns what it can do, without
// hardcoding anything.
// ---------------------------------------------------------------------------
static void test_universal_identity_request_is_answered_and_blinks() {
    Rig rig;
    rig.patches.boot(0);
    rig.midi.clear();

    const uint8_t request[] = {0xF0, SYSEX_UNIVERSAL_NON_REALTIME, SYSEX_BROADCAST_DEVICE,
                               SYSEX_GENERAL_INFORMATION, SYSEX_IDENTITY_REQUEST, 0xF7};
    rig.sysex.deliver_sysex(CONTROL, request, sizeof request, rig.now);

    TEST_ASSERT_EQUAL(1, rig.midi.sysex.size());
    const auto& r = rig.midi.sysex[0].bytes;
    TEST_ASSERT_EQUAL(0xF0, r[0]);
    TEST_ASSERT_EQUAL(SYSEX_UNIVERSAL_NON_REALTIME, r[1]);
    TEST_ASSERT_EQUAL(SYSEX_GENERAL_INFORMATION, r[3]);
    TEST_ASSERT_EQUAL(SYSEX_IDENTITY_REPLY, r[4]);
    TEST_ASSERT_EQUAL(SYSEX_MANUFACTURER, r[5]);
    TEST_ASSERT_EQUAL(0xF7, r[r.size() - 1]);

    // Both LEDs, so a user with two modules knows which one answered.
    bool green_alone = false, red_alone = false;
    for (uint32_t t = 0; t < (uint32_t)StatusLeds::IDENTIFY_BLINKS * StatusLeds::IDENTIFY_STEP_US; t += 10000) {
        rig.leds.service(t);
        if (rig.led_driver.levels[LED_GREEN] && !rig.led_driver.levels[LED_RED]) green_alone = true;
        if (rig.led_driver.levels[LED_RED] && !rig.led_driver.levels[LED_GREEN]) red_alone = true;
    }
    TEST_ASSERT_TRUE(green_alone);
    TEST_ASSERT_TRUE(red_alone);
}

static void test_a_message_for_another_device_is_ignored() {
    Rig rig;
    rig.patches.boot(0);
    rig.sysex.set_device_id(3);
    rig.midi.clear();

    const uint8_t m[] = {0xF0, SYSEX_MANUFACTURER, 5, SYSEX_HELLO, SYSEX_PROTOCOL_VERSION, 0xF7};
    rig.sysex.deliver_sysex(CONTROL, m, sizeof m, rig.now);
    TEST_ASSERT_EQUAL(0, rig.midi.sysex.size());

    // Its own id, and the broadcast id, both answer.
    const uint8_t mine[] = {0xF0, SYSEX_MANUFACTURER, 3, SYSEX_HELLO, SYSEX_PROTOCOL_VERSION, 0xF7};
    rig.sysex.deliver_sysex(CONTROL, mine, sizeof mine, rig.now);
    TEST_ASSERT_NOT_NULL(rig.midi.last_reply(SYSEX_IDENTITY));
}

static void test_an_older_protocol_version_fails_cleanly() {
    Rig rig;
    rig.patches.boot(0);
    const uint8_t m[] = {0xF0, SYSEX_MANUFACTURER, SYSEX_DEFAULT_DEVICE, SYSEX_HELLO,
                         (uint8_t)(SYSEX_PROTOCOL_VERSION + 1), 0xF7};
    rig.sysex.deliver_sysex(CONTROL, m, sizeof m, rig.now);
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_VERSION));
    TEST_ASSERT_NULL(rig.midi.last_reply(SYSEX_IDENTITY));
}

// A module that hears its own reply looped back through a host must not act
// on it, or two modules on one bus talk each other into a loop.
static void test_a_reply_looped_back_is_not_acted_on() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_HELLO);
    const auto reply = rig.midi.sysex[0].bytes;
    rig.midi.clear();
    rig.sysex.deliver_sysex(CONTROL, reply.data(), (uint16_t)reply.size(), rig.now);
    TEST_ASSERT_EQUAL(0, rig.midi.sysex.size());
}

static void test_capabilities_report_the_real_limits() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_CAPS_REQUEST);
    const auto* r = rig.midi.last_reply(SYSEX_CAPABILITIES);
    TEST_ASSERT_NOT_NULL(r);
    const auto& b = r->bytes;
    TEST_ASSERT_EQUAL(N_NODE, b[5]);
    TEST_ASSERT_EQUAL(MAX_IN, b[6]);
    TEST_ASSERT_EQUAL(MAX_OUT, b[7]);
    TEST_ASSERT_EQUAL(N_PARAM, (uint16_t)(b[8] | (b[9] << 7)));
    TEST_ASSERT_EQUAL(N_GATE_BUS, b[10]);
    TEST_ASSERT_EQUAL(N_NOTE_BUS, b[11]);
}

// The dump matches the compiled table exactly: an algorithm added to the
// firmware appears in an editor with no editor change.
static void test_the_algorithm_dump_matches_the_registry() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_ALGO_REQUEST);
    TEST_ASSERT_EQUAL(registry::count(), rig.midi.count_replies(SYSEX_ALGORITHM));

    size_t seen = 0;
    for (const auto& s : rig.midi.sysex) {
        if (s.bytes[3] != SYSEX_ALGORITHM) continue;
        const AlgorithmDescriptor* d = registry::at(s.bytes[5]);
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_EQUAL(registry::count(), s.bytes[6]);
        TEST_ASSERT_EQUAL(d->id, s.bytes[7]);
        TEST_ASSERT_EQUAL(d->n_in, s.bytes[8]);
        TEST_ASSERT_EQUAL(d->min_in, s.bytes[9]);
        TEST_ASSERT_EQUAL(d->n_out, s.bytes[10]);
        TEST_ASSERT_EQUAL(d->n_params, (uint16_t)(s.bytes[11] | (s.bytes[12] << 7)));

        // Then the domains, the algorithm's name, one name per inlet, one per
        // outlet, and the summary - the metadata that is the difference
        // between an editor that says "advance" and one that says "in 0".
        size_t at = 13 + 1u + d->n_in + d->n_out;          // wants_tick, domains
        auto read_string = [&](char* out, size_t capacity) {
            const uint8_t n = s.bytes[at++];
            TEST_ASSERT_TRUE(n < capacity);
            for (uint8_t k = 0; k < n; k++) out[k] = (char)s.bytes[at + k];
            out[n] = '\0';
            at += n;
        };
        char text[128];
        read_string(text, sizeof text);
        TEST_ASSERT_EQUAL_STRING(d->name, text);
        for (uint8_t k = 0; k < d->n_in; k++) {
            read_string(text, sizeof text);
            TEST_ASSERT_EQUAL_STRING(d->in_name[k], text);
        }
        for (uint8_t k = 0; k < d->n_out; k++) {
            read_string(text, sizeof text);
            TEST_ASSERT_EQUAL_STRING(d->out_name[k], text);
        }
        read_string(text, sizeof text);
        TEST_ASSERT_EQUAL_STRING(d->summary, text);
        TEST_ASSERT_EQUAL(d->category, s.bytes[at++]);
        // Nothing but the terminator is left: the record is exactly this
        // shape, which is what lets an editor parse it without guessing.
        TEST_ASSERT_EQUAL(s.bytes.size() - 1u, at);
        TEST_ASSERT_EQUAL(0xF7, s.bytes[at]);
        seen++;
    }
    TEST_ASSERT_EQUAL(registry::count(), seen);
}

static void test_parameter_descriptors_are_reported_per_group() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_PARAM_REQUEST, {ALGO_EUCLID_SEQ});

    const AlgorithmDescriptor* d = registry::find(ALGO_EUCLID_SEQ);
    size_t expected = 0;
    for (uint8_t g = 0; g < d->n_param_groups; g++) expected += d->param_groups[g].n_fields;
    TEST_ASSERT_EQUAL(expected, rig.midi.count_replies(SYSEX_PARAM_DESC));

    // A poly sequencer's 336 parameters are a handful of messages, not 336:
    // the groups are what make enumeration affordable over DIN.
    rig.midi.clear();
    rig.send(SYSEX_PARAM_REQUEST, {ALGO_POLY_SEQ});
    TEST_ASSERT_TRUE(rig.midi.count_replies(SYSEX_PARAM_DESC) < 32);

    rig.midi.clear();
    rig.send(SYSEX_PARAM_REQUEST, {200});
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_ARGUMENT));
}

// An algorithm with no parameters still answers. Sending nothing at all would
// be indistinguishable from a module that has gone away, and a host walking
// the registry to build its panels would stall on the first logic gate.
static void test_an_algorithm_with_no_parameters_still_answers() {
    Rig rig;
    rig.patches.boot(0);

    const AlgorithmDescriptor* gate = registry::find(ALGO_LOGIC_NOT);
    TEST_ASSERT_EQUAL_MESSAGE(0, gate->n_params, "a logic gate has no parameters");

    rig.midi.clear();
    rig.send(SYSEX_PARAM_REQUEST, {ALGO_LOGIC_NOT});
    TEST_ASSERT_EQUAL(0, rig.midi.count_replies(SYSEX_PARAM_DESC));
    TEST_ASSERT_TRUE_MESSAGE(rig.acked(), "silence is not an answer");

    // Walking the whole registry answers for every algorithm, which is what
    // an editor does on connect.
    for (uint8_t i = 0; i < registry::count(); i++) {
        rig.midi.clear();
        rig.send(SYSEX_PARAM_REQUEST, {registry::at(i)->id});
        const bool answered = rig.midi.count_replies(SYSEX_PARAM_DESC) > 0 || rig.acked();
        TEST_ASSERT_TRUE_MESSAGE(answered, registry::at(i)->name);
    }
}

// ---------------------------------------------------------------------------
// Bulk transfer.
// ---------------------------------------------------------------------------
static void test_a_patch_round_trips_through_dump_and_load() {
    Rig rig;
    GlobalSettings g = default_globals();
    g.bpm = 101;
    rig.patches.apply(two_node_patch(), g, 0);

    rig.midi.clear();
    rig.send(SYSEX_DUMP_REQUEST);
    const std::vector<uint8_t> image = reassemble(rig.midi);
    TEST_ASSERT_TRUE(image.size() > 0);

    // Load it back into a second module and get the same graph.
    Rig other;
    other.patches.boot(0);
    other.midi.clear();
    send_image(other, image);
    TEST_ASSERT_TRUE(other.acked());
    TEST_ASSERT_TRUE(patches_equal(rig.patches.active(), other.patches.active()));
    TEST_ASSERT_EQUAL(101, other.master.clock().bpm());
}

static void test_a_truncated_transfer_leaves_the_active_patch_alone() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    // Build a big enough image that it needs more than one chunk.
    Patch big = two_node_patch();
    big.nodes[0] = node_config(ALGO_POLY_SEQ);
    big.nodes[0].in_bus[0] = 0;
    big.nodes[0].out_bus[0] = 0;
    for (uint16_t p = 16; p < 300; p++) big.nodes[0].params[p] = (uint8_t)(p & 0x7F) | 1u;
    static uint8_t buffer[PATCH_SLOT_BYTES];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(big, g, buffer, sizeof buffer, written));
    const std::vector<uint8_t> image(buffer, buffer + written);
    TEST_ASSERT_TRUE(written > SYSEX_CHUNK_PAYLOAD);

    // Every chunk but the last: the transfer never completes.
    size_t at = 0;
    uint8_t seq = 0;
    while (at + SYSEX_CHUNK_PAYLOAD < image.size()) {
        uint8_t packed[256];
        const size_t packed_len = sysex::pack(&image[at], SYSEX_CHUNK_PAYLOAD, packed, sizeof packed);
        std::vector<uint8_t> args{seq, (uint8_t)(at == 0 ? SYSEX_CHUNK_FIRST : 0),
                                  sysex::checksum(packed, packed_len)};
        for (size_t i = 0; i < packed_len; i++) args.push_back(packed[i]);
        rig.send(SYSEX_PATCH_CHUNK_IN, args);
        at += SYSEX_CHUNK_PAYLOAD;
        seq = (uint8_t)((seq + 1u) & 0x7F);
    }
    TEST_ASSERT_TRUE(rig.sysex.transfer_in_progress());
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);

    // And the timeout abandons it without touching anything.
    rig.sysex.service(SYSEX_TRANSFER_TIMEOUT_US + 1u);
    TEST_ASSERT_FALSE(rig.sysex.transfer_in_progress());
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);
}

static void test_a_corrupted_chunk_is_caught_by_its_checksum() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    static uint8_t buffer[PATCH_SLOT_BYTES];
    size_t written = 0;
    patch_codec::encode(default_patch(), g, buffer, sizeof buffer, written);
    rig.midi.clear();
    send_image(rig, std::vector<uint8_t>(buffer, buffer + written), true);

    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_CHUNK));
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);
}

static void test_a_lost_chunk_abandons_the_transfer() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    Patch big = default_patch();
    big.nodes[0] = node_config(ALGO_POLY_SEQ);
    big.nodes[0].in_bus[0] = 0;
    big.nodes[0].out_bus[0] = 0;
    for (uint16_t p = 16; p < 320; p++) big.nodes[0].params[p] = (uint8_t)((p & 0x7F) | 1u);
    static uint8_t buffer[PATCH_SLOT_BYTES];
    size_t written = 0;
    patch_codec::encode(big, g, buffer, sizeof buffer, written);

    rig.midi.clear();
    send_image(rig, std::vector<uint8_t>(buffer, buffer + written), false, true);
    TEST_ASSERT_TRUE(rig.ever_naked_with(SYSEX_ERR_BAD_CHUNK));
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);
}

static void test_a_chunk_with_no_transfer_running_is_refused() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_PATCH_CHUNK_IN, {1, 0, 0});
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_NO_TRANSFER));
}

// The acceptance criterion that matters most: garbage, then a good patch.
static void test_garbage_cannot_stop_the_next_valid_patch_landing() {
    Rig rig;
    rig.patches.boot(0);
    GlobalSettings g = default_globals();

    // Every way this can go wrong, in a row.
    rig.send(0x7E);                                        // no such command
    rig.send(SYSEX_PATCH_CHUNK_IN, {9, 0, 99, 1, 2, 3});   // a chunk from nowhere
    rig.send(SYSEX_SET_PARAM, {200, 0, 0, 0});             // no such node
    rig.send(SYSEX_SLOT_LOAD, {99});                       // no such slot
    const uint8_t rubbish[] = {0xF0, SYSEX_MANUFACTURER, 0, SYSEX_PATCH_CHUNK_IN,
                               SYSEX_PROTOCOL_VERSION, 0x7F, 0x7F, 0x7F, 0x7F, 0xF7};
    rig.sysex.deliver_sysex(CONTROL, rubbish, sizeof rubbish, rig.now);

    // A malformed patch image, which decodes but does not validate.
    Patch bad = empty_patch();
    bad.nodes[0] = node_config(ALGO_ARPEGGIATOR);          // required inlets unconnected
    bad.n_nodes = 1;
    static uint8_t buffer[PATCH_SLOT_BYTES];
    size_t written = 0;
    patch_codec::encode(bad, g, buffer, sizeof buffer, written);
    send_image(rig, std::vector<uint8_t>(buffer, buffer + written));
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_REJECTED));

    // Now a good one. It has to land.
    rig.midi.clear();
    patch_codec::encode(two_node_patch(), g, buffer, sizeof buffer, written);
    send_image(rig, std::vector<uint8_t>(buffer, buffer + written));
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_TRUE(patches_equal(two_node_patch(), rig.patches.active()));
}

// ---------------------------------------------------------------------------
// Incremental edits.
// ---------------------------------------------------------------------------
static void test_one_bus_change_leaves_every_other_node_undisturbed() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    // Run the sequencer a few steps so it has a position to lose.
    StepSequencer* seq = static_cast<StepSequencer*>(rig.master.node(1));
    uint32_t now = 0;
    for (int i = 0; i < 5; i++) {
        rig.master.pass(now); now += 1000;
    }
    const uint32_t steps_before = seq->steps_taken();

    // Re-route node 0's output. Only node 0 is reconstructed.
    rig.send(SYSEX_SET_CONNECTION, {0, 1, 0, 3});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(3, rig.patches.active().nodes[0].out_bus[0]);
    TEST_ASSERT_EQUAL(steps_before, seq->steps_taken());
    TEST_ASSERT_EQUAL_PTR(seq, rig.master.node(1));       // the same object

    // 0x7F on the wire means "disconnect".
    rig.send(SYSEX_SET_CONNECTION, {0, 1, 0, 0x7F});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(NO_BUS, rig.patches.active().nodes[0].out_bus[0]);
}

static void test_an_invalid_connection_is_refused_and_changes_nothing() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    rig.send(SYSEX_SET_CONNECTION, {0, 0, 0, 100});       // no such note bus
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_REJECTED));
    TEST_ASSERT_EQUAL(0, rig.patches.active().nodes[0].in_bus[0]);

    rig.send(SYSEX_SET_CONNECTION, {9, 0, 0, 1});         // no such node
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_ARGUMENT));
}

static void test_a_parameter_edit_preserves_all_node_state() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    StepSequencer* seq = static_cast<StepSequencer*>(rig.master.node(1));

    uint32_t now = 0;
    for (int i = 0; i < 5; i++) { rig.master.pass(now); now += 1000; }
    const uint32_t before = seq->steps_taken();

    rig.send(SYSEX_SET_PARAM, {1, 0, 0, 16});             // node 1, param 0, length 16
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(16, seq->length());
    TEST_ASSERT_EQUAL(before, seq->steps_taken());
    TEST_ASSERT_EQUAL_PTR(seq, rig.master.node(1));

    // And it is readable back.
    rig.send(SYSEX_GET_PARAM, {1, 0, 0});
    const auto* r = rig.midi.last_reply(SYSEX_PARAM_VALUE);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(16, r->bytes[8]);
}

// A parameter byte reaches 255 and a SysEx data byte holds seven bits. The
// high byte of a step pattern *is* step 8, so truncating the value would not
// round it - it would turn step 8 off and clear the other seven with it.
static void test_a_parameter_above_127_survives_the_wire() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    // Node 1 is a StepSequencer; params[3] is steps 1-8, one bit per step.
    rig.send(SYSEX_SET_PARAM, {1, 3, 0, 0x01, 0x01});      // 0x81: steps 1 and 8
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(0x81, rig.patches.active().nodes[1].params[3]);

    rig.send(SYSEX_GET_PARAM, {1, 3, 0});
    const auto* r = rig.midi.last_reply(SYSEX_PARAM_VALUE);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(0x81, (uint16_t)(r->bytes[8] | (r->bytes[9] << 7)));
    // The low seven bits are where they always were, so a host that predates
    // the eighth bit reads exactly what it read before.
    TEST_ASSERT_EQUAL(0x01, r->bytes[8]);

    // And a host that sends four arguments still writes a 7-bit value.
    rig.send(SYSEX_SET_PARAM, {1, 3, 0, 0x7F});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(0x7F, rig.patches.active().nodes[1].params[3]);
}

static void test_a_parameter_beyond_its_range_is_refused() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.send(SYSEX_SET_PARAM, {1, 0, 0, 99});             // length max is 32
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_ARGUMENT));
    TEST_ASSERT_EQUAL(8, rig.patches.active().nodes[1].params[0]);
}

static void test_port_edits_reconstruct_nothing() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    Node* node0 = rig.master.node(0);

    rig.send(SYSEX_SET_GATE_PORT, {2, GATE_PORT_OUT, 5});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(GATE_PORT_OUT, rig.patches.active().gate_ports[2].direction);
    TEST_ASSERT_EQUAL(5, rig.patches.active().gate_ports[2].bus);
    TEST_ASSERT_EQUAL_PTR(node0, rig.master.node(0));

    // The MIDI port mask reaches 0x80, so its top bit rides in the direction
    // byte; check that round-trips.
    rig.send(SYSEX_SET_MIDI_PORT, {1, 0x02, 0x01, 3, 4});   // in, mask 0x81, ch 3, bus 4
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(0x81, rig.patches.active().midi_in[1].source_mask);
    TEST_ASSERT_EQUAL(3, rig.patches.active().midi_in[1].channel);
    TEST_ASSERT_EQUAL(4, rig.patches.active().midi_in[1].bus);

    rig.send(SYSEX_SET_GATE_PORT, {2, GATE_PORT_OUT, 99});
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_REJECTED));
}

// ---------------------------------------------------------------------------
// A patch swap under a held chord releases every note it started.
// ---------------------------------------------------------------------------
static void test_a_swap_under_a_held_chord_hangs_nothing() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    uint32_t now = 0;
    for (uint8_t note : {60, 64, 67, 71}) {
        const MidiEvent e = {MIDI_NOTE_ON, 1, note, 100};
        rig.master.deliver_midi(0x01, e);
    }
    for (int i = 0; i < 2; i++) { rig.master.pass(now); now += 1000; }

    // A whole different patch arrives mid-chord.
    static uint8_t buffer[PATCH_SLOT_BYTES];
    size_t written = 0;
    patch_codec::encode(default_patch(), g, buffer, sizeof buffer, written);
    send_image(rig, std::vector<uint8_t>(buffer, buffer + written));
    TEST_ASSERT_TRUE(rig.acked());
    for (int i = 0; i < 2; i++) { rig.master.pass(now); now += 1000; }

    int balance[128] = {0};
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_NOTE_ON && m.d2 > 0) balance[m.d1]++;
        else if (m.type == MIDI_NOTE_OFF || (m.type == MIDI_NOTE_ON && m.d2 == 0)) balance[m.d1]--;
    }
    for (int n = 0; n < 128; n++) TEST_ASSERT_EQUAL_MESSAGE(0, balance[n], "a swap hung a note");
}

// A connection change on a node holding notes releases them too: the handover
// runs at node scope exactly as it does at patch scope.
static void test_replacing_one_node_releases_the_notes_it_owned() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    uint32_t now = 0;
    for (uint8_t note : {60, 64, 67}) {
        const MidiEvent e = {MIDI_NOTE_ON, 1, note, 100};
        rig.master.deliver_midi(0x01, e);
    }
    for (int i = 0; i < 2; i++) { rig.master.pass(now); now += 1000; }
    rig.midi.clear();

    rig.send(SYSEX_SET_CONNECTION, {0, 1, 0, 3});
    for (int i = 0; i < 2; i++) { rig.master.pass(now); now += 1000; }

    uint8_t offs = 0;
    for (const auto& m : rig.midi.messages) {
        if (m.type == MIDI_NOTE_OFF || (m.type == MIDI_NOTE_ON && m.d2 == 0)) offs++;
    }
    TEST_ASSERT_EQUAL_MESSAGE(3, offs, "the replaced node must release what it was holding");
}

// ---------------------------------------------------------------------------
// Presets and Program Change.
// ---------------------------------------------------------------------------
static void test_slots_can_be_saved_listed_erased_and_recalled() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);

    rig.send(SYSEX_SLOT_SAVE, {2});
    TEST_ASSERT_TRUE(rig.acked());

    rig.midi.clear();
    rig.send(SYSEX_SLOT_LIST);
    const auto* list = rig.midi.last_reply(SYSEX_SLOTS);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL(PATCH_SLOTS, list->bytes[5]);
    TEST_ASSERT_EQUAL(1, list->bytes[6 + 2 * 3]);            // slot 2 occupied

    rig.patches.restore_defaults(1000);
    rig.send(SYSEX_SLOT_LOAD, {2});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_TRUE(patches_equal(two_node_patch(), rig.patches.active()));

    rig.send(SYSEX_SLOT_ERASE, {2});
    TEST_ASSERT_TRUE(rig.acked());
    rig.send(SYSEX_SLOT_LOAD, {2});
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_SLOT_EMPTY));
}

static void test_program_change_is_ignored_unless_it_is_addressed_to_us() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.patches.save_slot(1);
    rig.patches.restore_defaults(1000);

    // Recall is off by default: a Program Change meant for a downstream synth
    // must not switch the user's patch.
    TEST_ASSERT_FALSE(rig.sysex.program_change(0x01, 1, 1, 2000));
    TEST_ASSERT_EQUAL(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id);

    // Turned on, on channel 5, on port 0x01 only.
    g = rig.patches.globals();
    g.pc_enabled = 1;
    g.pc_channel = 5;
    g.pc_source_mask = 0x01;
    g.pc_quantise = SysexHandler::SWAP_IMMEDIATE;
    rig.patches.set_globals(g, 2000);
    rig.sysex.set_swap_timing(SysexHandler::SWAP_IMMEDIATE);

    TEST_ASSERT_FALSE(rig.sysex.program_change(0x01, 2, 1, 3000));   // wrong channel
    TEST_ASSERT_EQUAL(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id);
    TEST_ASSERT_FALSE(rig.sysex.program_change(0x10, 5, 1, 3000));   // wrong port
    TEST_ASSERT_EQUAL(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id);

    TEST_ASSERT_TRUE(rig.sysex.program_change(0x01, 5, 1, 4000));
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);
}

// A recall the module made itself is announced, so an editor follows along
// without polling.
static void test_a_recall_notifies_the_host() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.patches.save_slot(3);
    g = rig.patches.globals();
    g.pc_enabled = 1;
    g.pc_channel = 0;                    // omni
    g.pc_quantise = SysexHandler::SWAP_IMMEDIATE;
    rig.patches.set_globals(g, 0);

    rig.midi.clear();
    TEST_ASSERT_TRUE(rig.sysex.program_change(0x01, 9, 3, 1000));
    const auto* e = rig.midi.last_reply(SYSEX_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(SYSEX_EVENT_PROGRAM_CHANGE, e->bytes[5]);
    TEST_ASSERT_EQUAL(3, e->bytes[6]);
}

// Quantised recall waits for the boundary, and not a subtick less.
static void test_quantised_recall_swaps_on_the_next_bar_and_not_before() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.patches.save_slot(1);
    rig.patches.restore_defaults(0);

    g = rig.patches.globals();
    g.pc_enabled = 1;
    g.pc_channel = 0;
    g.pc_quantise = SysexHandler::SWAP_NEXT_BAR;
    rig.patches.set_globals(g, 0);

    rig.master.clock().start();
    // Somewhere in the middle of bar 0.
    for (uint32_t i = 0; i < CLOCK_SUBTICKS_PER_QUARTER + 7u; i++) rig.master.clock().advance();
    uint32_t count = 0;
    rig.master.clock().consume(count);

    TEST_ASSERT_TRUE(rig.sysex.program_change(0x01, 1, 1, 1000));
    TEST_ASSERT_TRUE(rig.sysex.swap_pending());
    rig.sysex.service(1000);
    TEST_ASSERT_EQUAL_MESSAGE(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id,
                              "the swap must not happen before the boundary");

    // Advance to just before the bar line: still nothing.
    const uint32_t bar = (uint32_t)CLOCK_SUBTICKS_PER_QUARTER * SysexHandler::BEATS_PER_BAR;
    while (rig.master.clock().count() + 1u < bar) rig.master.clock().advance();
    rig.sysex.service(2000);
    TEST_ASSERT_EQUAL(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id);

    // And now the bar line arrives.
    rig.master.clock().advance();
    rig.sysex.service(3000);
    TEST_ASSERT_FALSE(rig.sysex.swap_pending());
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);
}

// With the clock stopped, "the next bar" never arrives. A recall that never
// happened is worse than one that glitched.
static void test_a_quantised_recall_with_a_stopped_clock_is_immediate() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.patches.save_slot(1);
    rig.patches.restore_defaults(0);
    g = rig.patches.globals();
    g.pc_enabled = 1;
    g.pc_channel = 0;
    g.pc_quantise = SysexHandler::SWAP_NEXT_BAR;
    rig.patches.set_globals(g, 0);

    // The internal clock free-runs from boot, so stopping it is what makes
    // "the next bar" a boundary that never arrives.
    rig.master.clock().stop();
    TEST_ASSERT_FALSE(rig.master.clock().running());
    TEST_ASSERT_TRUE(rig.sysex.program_change(0x01, 1, 1, 1000));
    TEST_ASSERT_FALSE(rig.sysex.swap_pending());
    TEST_ASSERT_EQUAL(ALGO_TRANSPOSE, rig.patches.active().nodes[0].algorithm_id);
}

static void test_restore_defaults_over_sysex() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.send(SYSEX_RESTORE_DEFAULTS);
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(ALGO_SUSTAIN, rig.patches.active().nodes[0].algorithm_id);
}

static void test_globals_can_be_set_and_come_back_in_a_dump() {
    Rig rig;
    rig.patches.boot(0);
    rig.send(SYSEX_SET_GLOBALS, {MasterClock::CLOCK_INTERNAL, 2,
                                 (uint8_t)(150 & 0x7F), (uint8_t)(150 >> 7),
                                 1, 5, 0x01, SysexHandler::SWAP_NEXT_BEAT});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(150, rig.master.clock().bpm());
    TEST_ASSERT_EQUAL(2, rig.master.clock().cv_ppqn());
    TEST_ASSERT_EQUAL(1, rig.patches.globals().pc_enabled);
    TEST_ASSERT_EQUAL(5, rig.patches.globals().pc_channel);

    rig.send(SYSEX_SET_GLOBALS, {9, 2, 0, 1, 0, 0, 0, 0});     // no such clock source
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_ARGUMENT));
}

// The key (midi/global_scale.h) rides on the same message, appended: a host
// that predates it sends eight arguments and is not told its message is
// short, and the module is left in the key it was already in.
static void test_the_global_scale_travels_with_the_globals() {
    Rig rig;
    rig.patches.boot(0);
    const uint8_t was = global_scale::id();

    rig.send(SYSEX_SET_GLOBALS, {MasterClock::CLOCK_INTERNAL, 4,
                                 (uint8_t)(120 & 0x7F), (uint8_t)(120 >> 7),
                                 0, 1, 0, 0});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(was, global_scale::id());

    rig.send(SYSEX_SET_GLOBALS, {MasterClock::CLOCK_INTERNAL, 4,
                                 (uint8_t)(120 & 0x7F), (uint8_t)(120 >> 7),
                                 0, 1, 0, 0, SCALE_LYDIAN, 7});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(SCALE_LYDIAN, rig.patches.globals().scale);
    TEST_ASSERT_EQUAL(7, rig.patches.globals().root);
    TEST_ASSERT_EQUAL(SCALE_LYDIAN, global_scale::id());       // and it is live
    TEST_ASSERT_EQUAL(7, global_scale::root());

    rig.send(SYSEX_SET_GLOBALS, {MasterClock::CLOCK_INTERNAL, 4,
                                 (uint8_t)(120 & 0x7F), (uint8_t)(120 >> 7),
                                 0, 1, 0, 0, SCALE_COUNT, 0});  // no such scale
    TEST_ASSERT_TRUE(rig.naked_with(SYSEX_ERR_BAD_ARGUMENT));
    TEST_ASSERT_EQUAL(SCALE_LYDIAN, global_scale::id());
    global_scale::set(SCALE_CHROMATIC, 0);
}

// ---------------------------------------------------------------------------
// No heap allocation on the receive or swap path.
// ---------------------------------------------------------------------------
static void test_the_protocol_never_allocates() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(two_node_patch(), g, 0);
    rig.send(SYSEX_HELLO);                    // warm anything that would

    // Messages are built into a fixed buffer here rather than through the
    // std::vector helper, because the helper allocates and the firmware is
    // what is being measured.
    uint8_t message[16] = {0xF0, SYSEX_MANUFACTURER, SYSEX_DEFAULT_DEVICE, 0,
                           SYSEX_PROTOCOL_VERSION, 0, 0, 0, 0, 0xF7};
    rig.midi.clear();
    rig.midi.sysex.reserve(4096);

    const size_t before = g_allocations;
    for (int i = 0; i < 5; i++) {
        message[3] = SYSEX_SET_PARAM;
        message[5] = 1; message[6] = 0; message[7] = 0; message[8] = (uint8_t)(8 + i);
        rig.sysex.deliver_sysex(CONTROL, message, 10, rig.now);

        message[3] = SYSEX_SET_CONNECTION;
        message[5] = 0; message[6] = 1; message[7] = 0; message[8] = (uint8_t)(1 + (i % 3));
        rig.sysex.deliver_sysex(CONTROL, message, 10, rig.now);

        rig.sysex.service((uint32_t)(1000 * i));
    }

    // The only allocations left are the fake's own: it copies each reply into
    // a byte vector so a test can read it. One per reply, and no more.
    const size_t replies = rig.midi.sysex.size();
    TEST_ASSERT_TRUE(replies > 0);
    TEST_ASSERT_EQUAL_MESSAGE(replies, g_allocations - before,
                              "the protocol allocated beyond the fake's own recording");
}

// A dump is chunked, and each chunk stays inside the receive buffer any
// plausible host and library can hold.
static void test_dump_chunks_stay_within_the_wire_budget() {
    Rig rig;
    GlobalSettings g = default_globals();
    Patch big = two_node_patch();
    big.nodes[0] = node_config(ALGO_DRUM_SEQ_MIDI);
    big.nodes[0].in_bus[0] = 0;
    big.nodes[0].out_bus[0] = 0;
    // A full velocity grid: the widest thing a patch can carry, and every
    // byte inside its descriptor's range so the patch actually validates.
    for (uint16_t p = DrumSeqMidi::VELOCITY_BASE; p < DrumSeqMidi::PARAM_COUNT; p++){
        big.nodes[0].params[p] = (uint8_t)((p % 127u) + 1u);
    }
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(big, g, 0));

    rig.midi.clear();
    rig.send(SYSEX_DUMP_REQUEST);
    TEST_ASSERT_TRUE_MESSAGE(rig.midi.count_replies(SYSEX_PATCH_CHUNK_OUT) > 1,
                             "a wide patch has to be chunked");
    for (const auto& s : rig.midi.sysex) {
        if (s.bytes[3] != SYSEX_PATCH_CHUNK_OUT) continue;
        TEST_ASSERT_TRUE(s.bytes.size() <= SYSEX_TX_MAX);
        for (size_t i = 1; i + 1 < s.bytes.size(); i++) {
            TEST_ASSERT_TRUE_MESSAGE(s.bytes[i] <= 0x7F, "a chunk carried a byte with bit 7 set");
        }
    }
    // And it reassembles into an image that loads.
    const std::vector<uint8_t> image = reassemble(rig.midi);
    Rig other;
    other.patches.boot(0);
    send_image(other, image);
    TEST_ASSERT_TRUE(other.acked());
    TEST_ASSERT_EQUAL(ALGO_DRUM_SEQ_MIDI, other.patches.active().nodes[0].algorithm_id);
}


// ---------------------------------------------------------------------------
// Uptime. Every command arrives with the pass's clock, and everything a
// command starts is measured from it. These run the handler on a module that
// has been on for a while, because a module that has just booted is the one
// case where "measured from boot" and "measured from the message" agree.
// ---------------------------------------------------------------------------
static Patch big_patch() {
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_POLY_SEQ);
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 0;
    // Every step byte non-zero and legal for every field, so the image is
    // several chunks long and the validator accepts it at the end.
    for (uint16_t i = 16; i < 300; i++) p.nodes[0].params[i] = 1;
    p.n_nodes = 1;
    return p;
}

static void test_a_chunked_transfer_survives_the_main_loop_after_ten_seconds_of_uptime() {
    Rig rig;
    GlobalSettings g = default_globals();
    rig.patches.apply(empty_patch(), g, 0);
    static uint8_t buffer[PATCH_SLOT_BYTES];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(big_patch(), g, buffer, sizeof buffer, written));
    TEST_ASSERT_TRUE(written > SYSEX_CHUNK_PAYLOAD);

    rig.now = SYSEX_TRANSFER_TIMEOUT_US + 1000000u;      // eleven seconds in
    send_image(rig, std::vector<uint8_t>(buffer, buffer + written), false, false, true);
    TEST_ASSERT_FALSE_MESSAGE(rig.ever_naked_with(SYSEX_ERR_NO_TRANSFER),
                              "the timeout abandoned a transfer that was still arriving");
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_EQUAL(ALGO_POLY_SEQ, rig.patches.active().nodes[0].algorithm_id);
}

static void test_learn_over_sysex_binds_after_twenty_seconds_of_uptime() {
    Rig rig;
    GlobalSettings g = default_globals();
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_bus[0] = 0;
    p.n_nodes = 1;
    rig.patches.apply(p, g, 0);

    rig.now = CcMapper::LEARN_TIMEOUT_US + 5000000u;
    rig.send(SYSEX_CC_LEARN, {1, 0, CC_TARGET_NODE, 0, 1, 0});
    TEST_ASSERT_TRUE(rig.acked());
    TEST_ASSERT_TRUE(rig.cc.learning());
    TEST_ASSERT_TRUE_MESSAGE(rig.cc.observe(mmMIDI_USB_0, 1, 20, 64, rig.now + 1000),
                             "the learn had already timed out");
    TEST_ASSERT_EQUAL(20, rig.patches.active().cc_map[0].cc);
}

static void test_a_sysex_edit_is_autosaved_after_the_settle_time_not_at_once() {
    Rig rig;
    GlobalSettings g = default_globals();
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_CLOCK_DIV);
    p.nodes[0].out_bus[0] = 0;
    p.n_nodes = 1;
    rig.patches.apply(p, g, 0);
    rig.patches.service(PatchStore::AUTOSAVE_SETTLE_US + 1u);   // the load's own save
    TEST_ASSERT_FALSE(rig.store.dirty());

    rig.now = 30000000u;
    const uint32_t writes = rig.store.writes();
    rig.send(SYSEX_SET_PARAM, {0, 1, 0, 5});
    TEST_ASSERT_TRUE(rig.acked());
    rig.patches.service(rig.now + 1000);                         // the next loop
    TEST_ASSERT_EQUAL_MESSAGE(writes, rig.store.writes(), "flash was written on the very next loop");
    rig.patches.service(rig.now + PatchStore::AUTOSAVE_SETTLE_US + 1u);
    TEST_ASSERT_EQUAL(writes + 1u, rig.store.writes());
}

static void test_hello_blinks_the_identify_pattern_whatever_the_uptime() {
    Rig rig;
    rig.now = 60000000u;
    rig.send(SYSEX_HELLO);
    rig.leds.service(rig.now + 1000);
    TEST_ASSERT_EQUAL(StatusLeds::BRIGHT, rig.leds.level(LED_GREEN));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_seven_bit_packing_round_trips_every_byte_value);
    RUN_TEST(test_packing_refuses_a_buffer_that_is_too_small);
    RUN_TEST(test_universal_identity_request_is_answered_and_blinks);
    RUN_TEST(test_a_message_for_another_device_is_ignored);
    RUN_TEST(test_an_older_protocol_version_fails_cleanly);
    RUN_TEST(test_a_reply_looped_back_is_not_acted_on);
    RUN_TEST(test_capabilities_report_the_real_limits);
    RUN_TEST(test_the_algorithm_dump_matches_the_registry);
    RUN_TEST(test_parameter_descriptors_are_reported_per_group);
    RUN_TEST(test_an_algorithm_with_no_parameters_still_answers);
    RUN_TEST(test_a_patch_round_trips_through_dump_and_load);
    RUN_TEST(test_a_truncated_transfer_leaves_the_active_patch_alone);
    RUN_TEST(test_a_corrupted_chunk_is_caught_by_its_checksum);
    RUN_TEST(test_a_lost_chunk_abandons_the_transfer);
    RUN_TEST(test_a_chunk_with_no_transfer_running_is_refused);
    RUN_TEST(test_garbage_cannot_stop_the_next_valid_patch_landing);
    RUN_TEST(test_one_bus_change_leaves_every_other_node_undisturbed);
    RUN_TEST(test_an_invalid_connection_is_refused_and_changes_nothing);
    RUN_TEST(test_a_parameter_edit_preserves_all_node_state);
    RUN_TEST(test_a_parameter_above_127_survives_the_wire);
    RUN_TEST(test_a_parameter_beyond_its_range_is_refused);
    RUN_TEST(test_port_edits_reconstruct_nothing);
    RUN_TEST(test_a_swap_under_a_held_chord_hangs_nothing);
    RUN_TEST(test_replacing_one_node_releases_the_notes_it_owned);
    RUN_TEST(test_slots_can_be_saved_listed_erased_and_recalled);
    RUN_TEST(test_program_change_is_ignored_unless_it_is_addressed_to_us);
    RUN_TEST(test_a_recall_notifies_the_host);
    RUN_TEST(test_quantised_recall_swaps_on_the_next_bar_and_not_before);
    RUN_TEST(test_a_quantised_recall_with_a_stopped_clock_is_immediate);
    RUN_TEST(test_restore_defaults_over_sysex);
    RUN_TEST(test_globals_can_be_set_and_come_back_in_a_dump);
    RUN_TEST(test_the_global_scale_travels_with_the_globals);
    RUN_TEST(test_dump_chunks_stay_within_the_wire_budget);
    RUN_TEST(test_the_protocol_never_allocates);
    RUN_TEST(test_a_chunked_transfer_survives_the_main_loop_after_ten_seconds_of_uptime);
    RUN_TEST(test_learn_over_sysex_binds_after_twenty_seconds_of_uptime);
    RUN_TEST(test_a_sysex_edit_is_autosaved_after_the_settle_time_not_at_once);
    RUN_TEST(test_hello_blinks_the_identify_pattern_whatever_the_uptime);
    return UNITY_END();
}
