#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "algorithm/sequencer/sequencers.h"
#include "algorithm/sequencer/note_sequencer.h"
#include "algorithm/sequencer/drum_sequencer.h"
#include "algorithm/clock/clock_div.h"
#include "algorithm/midi/transpose.h"
#include "algorithm/midi/note_quantise.h"
#include "algorithm/midi/chord.h"
#include "midi/scale.h"

// Heap instrumentation, as in test_master: set_param must never allocate.
static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// The parameter seam (#20). Everything here goes through
// MixedModeMaster::set_node_param, which is the one entry point #11's SysEx
// edits, #21's CC mapping and the console all use.

static NodeConfig node(uint8_t id, uint8_t in0, uint8_t out0) {
    NodeConfig c = node_config(id);
    c.in_bus[0] = in0;
    c.out_bus[0] = out0;
    return c;
}

static void run_passes(MixedModeMaster& m, int n, uint32_t& now, uint32_t step_us = 1000) {
    for (int i = 0; i < n; i++) { m.pass(now); now += step_us; }
}

// ---------------------------------------------------------------------------
// Descriptors: every algorithm's parameter space is described, and the
// descriptors are internally consistent. This is the check that catches an
// algorithm gaining a parameter without gaining a descriptor - the hole that
// would leave #12's editor unable to draw a control for it.
// ---------------------------------------------------------------------------
static void test_every_parameter_of_every_algorithm_is_described() {
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        uint32_t covered = 0;
        for (uint8_t g = 0; g < d->n_param_groups; g++) {
            const ParamGroup& grp = d->param_groups[g];
            TEST_ASSERT_NOT_NULL_MESSAGE(grp.fields, d->name);
            TEST_ASSERT_TRUE_MESSAGE(grp.repeat > 0 && grp.n_fields > 0, d->name);
            covered += (uint32_t)grp.repeat * grp.n_fields;
            // No group may describe a parameter the algorithm does not have.
            TEST_ASSERT_TRUE_MESSAGE(grp.first + (uint32_t)grp.repeat * grp.n_fields <= d->n_params, d->name);
            for (uint16_t f = 0; f < grp.n_fields; f++) {
                const ParamDescriptor& p = grp.fields[f];
                TEST_ASSERT_NOT_NULL_MESSAGE(p.name, d->name);
                TEST_ASSERT_TRUE_MESSAGE(p.min <= p.max, p.name);
                // A default outside the range would make the zero-means-default
                // rule produce a value the validator would reject.
                TEST_ASSERT_TRUE_MESSAGE(p.def == 0 || (p.def >= p.min && p.def <= p.max), p.name);
                if (p.kind == PARAM_ENUM) TEST_ASSERT_NOT_NULL_MESSAGE(p.options, p.name);
            }
        }
        char msg[96];
        snprintf(msg, sizeof msg, "%s describes %u of %u parameters",
                 d->name, (unsigned)covered, (unsigned)d->n_params);
        TEST_ASSERT_EQUAL_MESSAGE(d->n_params, covered, msg);
    }
}

// Every inlet, every outlet and every algorithm is named and described.
//
// An algorithm whose ports are only numbered is one a user has to read the
// source to patch: "in 0" and "in 1" do not say which one advances the
// sequencer and which one resets it. This is the same check
// test_every_parameter_of_every_algorithm_is_described makes for #20's
// parameter names, at port scope - an algorithm added to the firmware fails
// here until it says what its connections mean.
static void test_every_port_and_algorithm_is_named() {
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        TEST_ASSERT_NOT_NULL_MESSAGE(d->summary, d->name);
        TEST_ASSERT_TRUE_MESSAGE(d->summary[0] != '\0', d->name);
        // The reply that carries these is bounded (SYSEX_TX_MAX), so the
        // budget is checked here rather than discovered as a truncated
        // descriptor on a module.
        TEST_ASSERT_TRUE_MESSAGE(strlen(d->summary) <= 96, d->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(d->in_name, d->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(d->out_name, d->name);
        for (uint8_t k = 0; k < d->n_in && k < MAX_IN; k++) {
            TEST_ASSERT_NOT_NULL_MESSAGE(d->in_name[k], d->name);
            TEST_ASSERT_TRUE_MESSAGE(d->in_name[k][0] != '\0', d->name);
            TEST_ASSERT_TRUE_MESSAGE(strlen(d->in_name[k]) <= 16, d->in_name[k]);
        }
        for (uint8_t k = 0; k < d->n_out && k < MAX_OUT; k++) {
            TEST_ASSERT_NOT_NULL_MESSAGE(d->out_name[k], d->name);
            TEST_ASSERT_TRUE_MESSAGE(d->out_name[k][0] != '\0', d->name);
            TEST_ASSERT_TRUE_MESSAGE(strlen(d->out_name[k]) <= 16, d->out_name[k]);
        }
    }
}

// Enum descriptors must name every value they admit, or an editor renders a
// blank entry for one.
static void test_enum_descriptors_name_every_option() {
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        for (uint16_t p = 0; p < d->n_params; p++) {
            const ParamDescriptor* pd = registry::param(*d, p);
            if (pd == nullptr || pd->kind != PARAM_ENUM) continue;
            for (uint16_t v = pd->min; v <= pd->max; v++) {
                TEST_ASSERT_NOT_NULL_MESSAGE(pd->options[v - pd->min], pd->name);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The bounds contract, for **every** algorithm and **every** parameter: min
// and max are accepted and read back, one value past each bound is rejected,
// and a rejected write leaves the previous value alone.
// ---------------------------------------------------------------------------
static void test_every_parameter_accepts_its_bounds_and_rejects_beyond() {
    for (uint8_t i = 0; i < registry::count(); i++) {
        const AlgorithmDescriptor* d = registry::at(i);
        if (d->n_params == 0) continue;

        FakeGpio gpio; RecordingMidiOut midi;
        MixedModeMaster master(gpio, midi);
        Patch patch = empty_patch();
        patch.nodes[0] = node_config(d->id);
        // Connect every inlet and outlet the algorithm has, on bus 0 of its
        // own domain, so a required inlet does not fail the load.
        for (uint8_t in = 0; in < d->n_in && in < MAX_IN; in++) patch.nodes[0].in_bus[in] = 0;
        for (uint8_t out = 0; out < d->n_out && out < MAX_OUT; out++) patch.nodes[0].out_bus[out] = 0;
        patch.n_nodes = 1;
        TEST_ASSERT_EQUAL_MESSAGE(LOAD_OK, master.load(patch), d->name);
        master.setup();

        for (uint16_t p = 0; p < d->n_params; p++) {
            const ParamDescriptor* pd = registry::param(*d, p);
            if (pd == nullptr) continue;
            char msg[96];
            snprintf(msg, sizeof msg, "%s param %u (%s)", d->name, (unsigned)p, pd->name);

            // A parameter pinned to a single value (a reserved byte) has no
            // bounds to exercise.
            if (pd->min == pd->max && pd->min == 0) {
                TEST_ASSERT_EQUAL_MESSAGE(PARAM_VALUE_OUT_OF_RANGE,
                                          master.set_node_param(0, p, 1), msg);
                continue;
            }

            // min and max are accepted, and read back as what is running.
            if (pd->min != 0) {
                TEST_ASSERT_EQUAL_MESSAGE(PARAM_SET_OK, master.set_node_param(0, p, pd->min), msg);
                uint8_t got = 0;
                TEST_ASSERT_TRUE_MESSAGE(master.get_node_param(0, p, got), msg);
                TEST_ASSERT_EQUAL_MESSAGE(pd->min, got, msg);
            }
            TEST_ASSERT_EQUAL_MESSAGE(PARAM_SET_OK, master.set_node_param(0, p, pd->max), msg);
            uint8_t at_max = 0;
            TEST_ASSERT_TRUE_MESSAGE(master.get_node_param(0, p, at_max), msg);
            TEST_ASSERT_EQUAL_MESSAGE(pd->max, at_max, msg);

            // One past the top is rejected and the node keeps the old value.
            if (pd->max < 255) {
                TEST_ASSERT_EQUAL_MESSAGE(PARAM_VALUE_OUT_OF_RANGE,
                                          master.set_node_param(0, p, (uint8_t)(pd->max + 1)), msg);
                uint8_t after = 0;
                master.get_node_param(0, p, after);
                TEST_ASSERT_EQUAL_MESSAGE(pd->max, after, msg);
            }
            // One below the bottom, where there is one. Zero is never
            // out of range - it means the default - so a min of 1 has no
            // value below it to reject.
            if (pd->min > 1) {
                TEST_ASSERT_EQUAL_MESSAGE(PARAM_VALUE_OUT_OF_RANGE,
                                          master.set_node_param(0, p, (uint8_t)(pd->min - 1)), msg);
            }
        }
        // An index past the end is not a parameter at all.
        TEST_ASSERT_EQUAL(PARAM_NO_SUCH_PARAM, master.set_node_param(0, d->n_params, 1));
    }
}

static void test_unknown_node_is_rejected() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node(ALGO_TRANSPOSE, 0, 0);
    p.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    TEST_ASSERT_EQUAL(PARAM_NO_SUCH_NODE, master.set_node_param(1, 0, 5));
    uint8_t v = 0;
    TEST_ASSERT_FALSE(master.get_node_param(1, 0, v));
}

// ---------------------------------------------------------------------------
// Euclid: the acceptance case from #20 - turning the pulse count is the most
// obvious thing anyone will want to do with this module, and until now it
// was impossible.
// ---------------------------------------------------------------------------
static void test_euclid_pulses_change_the_pattern_without_moving_the_cursor() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_EUCLID_SEQ);
    p.nodes[0].in_bus[0] = 0;          // advance
    p.nodes[0].out_bus[0] = 1;
    p.nodes[0].params[0] = 8;          // 8 steps
    p.nodes[0].params[3] = 3;          // E(3,8)
    p.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    EuclidianSequencer* seq = static_cast<EuclidianSequencer*>(master.node(0));
    const uint32_t e38 = seq->pattern();

    uint32_t now = 0;
    run_passes(master, 3, now);
    const uint8_t cursor_before = seq->position();
    const uint32_t steps_before = seq->steps_taken();

    TEST_ASSERT_EQUAL(PARAM_SET_OK, master.set_node_param(0, 3, 5));
    TEST_ASSERT_EQUAL(5, seq->pulses());
    TEST_ASSERT_NOT_EQUAL(e38, seq->pattern());
    // E(5,8) has five pulses over eight steps.
    uint8_t on = 0;
    for (uint8_t s = 0; s < 8; s++) if (seq->on(s)) on++;
    TEST_ASSERT_EQUAL(5, on);
    // The transport is untouched: the pattern changed under a running
    // sequence, the sequence did not restart.
    TEST_ASSERT_EQUAL(cursor_before, seq->position());
    TEST_ASSERT_EQUAL(steps_before, seq->steps_taken());
    run_passes(master, 2, now);
}

// A length change clamps the cursor on the next advance rather than jumping
// immediately, which is what keeps a running sequence from stumbling.
static void test_shortening_a_sequence_clamps_on_the_next_advance() {
    NodeConfig c = node_config(ALGO_STEP_SEQ);
    c.in_bus[0] = 0;
    c.out_bus[0] = 1;
    c.params[0] = 16;
    c.params[3] = 0xFF; c.params[4] = 0xFF;    // steps 1..16 all on
    StepSequencer seq(c);

    BusManager bus;
    for (int i = 0; i < 10; i++) {             // advance to step 9
        bus.gate_write(0, true);  bus.swap(); seq.process(bus, 0); bus.swap();
        bus.gate_write(0, false); bus.swap(); seq.process(bus, 0); bus.swap();
    }
    TEST_ASSERT_EQUAL(9, seq.position());

    TEST_ASSERT_TRUE(seq.set_param(0, 4));     // now four steps long
    TEST_ASSERT_EQUAL(4, seq.length());
    TEST_ASSERT_EQUAL(9, seq.position());      // not immediately

    bus.gate_write(0, true); bus.swap(); seq.process(bus, 0); bus.swap();
    TEST_ASSERT_TRUE(seq.position() < 4);      // clamped on the advance
}

// The pattern block round-trips: a step beyond the current length is stored,
// not masked away, so shortening and lengthening from a knob is lossless.
static void test_step_pattern_bytes_round_trip() {
    NodeConfig c = node_config(ALGO_STEP_SEQ);
    c.in_bus[0] = 0; c.out_bus[0] = 1;
    c.params[0] = 4;
    StepSequencer seq(c);
    TEST_ASSERT_TRUE(seq.set_param(6, 0xA5));      // steps 25..32
    TEST_ASSERT_EQUAL(0xA5, seq.get_param(6));
    TEST_ASSERT_TRUE(seq.set_param(0, MAX_SEQUENCE_LEN));
    TEST_ASSERT_TRUE(seq.on(24));                  // bit 0 of 0xA5
    TEST_ASSERT_FALSE(seq.on(25));
}

// ---------------------------------------------------------------------------
// ClockDiv: the amount re-derives the period, and the multiply refusal is
// re-checked rather than frozen at construction.
// ---------------------------------------------------------------------------
static void test_clock_div_amount_rederives_the_period() {
    NodeConfig c = node_config(ALGO_CLOCK_DIV);
    c.out_bus[0] = 0;
    c.params[1] = 4;                                  // /4 from the tick source
    ClockDiv div(c);
    TEST_ASSERT_EQUAL_UINT32(4u * CLOCK_SUBTICK, div.period());

    TEST_ASSERT_TRUE(div.set_param(1, 8));
    TEST_ASSERT_EQUAL_UINT32(8u * CLOCK_SUBTICK, div.period());
    TEST_ASSERT_EQUAL(8, div.get_param(1));

    // Switching to multiply re-derives too, and an exact multiplier is taken.
    TEST_ASSERT_TRUE(div.set_param(1, 4));
    TEST_ASSERT_TRUE(div.set_param(0, 1));
    TEST_ASSERT_FALSE(div.multiply_refused());
    TEST_ASSERT_EQUAL_UINT32(CLOCK_SUBTICK / 4u, div.period());

    // A multiplier that does not divide the subdivision is still refused.
    TEST_ASSERT_TRUE(div.set_param(1, 5));
    TEST_ASSERT_TRUE(div.multiply_refused());
}

static void test_clock_div_multiply_from_a_gate_is_still_refused() {
    NodeConfig c = node_config(ALGO_CLOCK_DIV);
    c.in_bus[0] = 0;                                  // gate source
    c.out_bus[0] = 1;
    c.params[1] = 2;
    ClockDiv div(c);
    TEST_ASSERT_TRUE(div.gate_sourced());
    TEST_ASSERT_FALSE(div.multiply_refused());

    TEST_ASSERT_TRUE(div.set_param(0, 1));            // ask for multiply
    TEST_ASSERT_TRUE(div.multiply_refused());         // still refused, runs x1
    TEST_ASSERT_EQUAL_UINT32(1u, div.period());
}

// The pulse already scheduled is not moved: the current period completes and
// the new one applies from the pulse after it.
static void test_clock_div_amount_change_lets_the_current_period_complete() {
    BusManager bus;
    NodeConfig c = node_config(ALGO_CLOCK_DIV);
    c.out_bus[0] = 0;
    c.params[1] = 4;                                  // a pulse every 4 ticks
    ClockDiv div(c);

    div.tick(bus, 0);                                 // fires at subtick 0
    TEST_ASSERT_EQUAL_UINT32(1u, div.pulses());
    div.set_param(1, 8);                              // now /8
    // The pulse scheduled for subtick 4*CLOCK_SUBTICK still lands there.
    div.tick(bus, 4u * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(2u, div.pulses());
    // The one after it is a full new period away, not four ticks.
    div.tick(bus, 4u * CLOCK_SUBTICK + 4u * CLOCK_SUBTICK - 1u);
    TEST_ASSERT_EQUAL_UINT32(2u, div.pulses());
    div.tick(bus, 12u * CLOCK_SUBTICK);
    TEST_ASSERT_EQUAL_UINT32(3u, div.pulses());
}

// ---------------------------------------------------------------------------
// The validator (#20): a patch carrying an out-of-range parameter is rejected
// whole, and the patch that was running keeps running.
// ---------------------------------------------------------------------------
static void test_out_of_range_parameter_is_rejected_by_the_validator() {
    NodeConfig c = node_config(ALGO_ARPEGGIATOR);
    c.in_bus[0] = 0; c.in_bus[1] = 0; c.out_bus[0] = 0;
    c.params[1] = 9;                                  // octaves, max 4
    TEST_ASSERT_EQUAL(CONFIG_PARAM_OUT_OF_RANGE, registry::validate(c));
    TEST_ASSERT_EQUAL(1, registry::last_bad_param());

    c.params[1] = 4;
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
    c.params[1] = 0;                                  // zero means the default
    TEST_ASSERT_EQUAL(CONFIG_OK, registry::validate(c));
}

static void test_a_bad_parameter_leaves_the_running_patch_alone() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);

    Patch good = empty_patch();
    good.nodes[0] = node(ALGO_TRANSPOSE, 0, 1);
    good.nodes[0].params[0] = 12;
    good.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(good));
    master.setup();

    Patch bad = empty_patch();
    bad.nodes[0] = node_config(ALGO_NOTE_QUANTISE);
    bad.nodes[0].in_bus[0] = 0; bad.nodes[0].out_bus[0] = 1;
    bad.nodes[0].params[0] = SCALE_COUNT + 3;         // no such scale
    bad.n_nodes = 1;
    TEST_ASSERT_EQUAL(LOAD_NODE_INVALID, master.load(bad));
    TEST_ASSERT_EQUAL(CONFIG_PARAM_OUT_OF_RANGE, master.last_node_error());

    // Still the Transpose, still at +12.
    TEST_ASSERT_EQUAL(1, master.node_count());
    uint8_t v = 0;
    TEST_ASSERT_TRUE(master.get_node_param(0, 0, v));
    TEST_ASSERT_EQUAL(12, v);
}

// ---------------------------------------------------------------------------
// Note-off ownership under a parameter change (#10 at parameter scope). Each
// of these moves a parameter while a chord is held and asserts every note-on
// gets its matching note-off at the pitch it was sent at.
// ---------------------------------------------------------------------------
static void note_on(MixedModeMaster& m, uint8_t note, uint8_t vel = 100) {
    const MidiEvent e = {MIDI_NOTE_ON, 1, note, vel};
    m.deliver_midi(1, e);
}
static void note_off(MixedModeMaster& m, uint8_t note) {
    const MidiEvent e = {MIDI_NOTE_OFF, 1, note, 0};
    m.deliver_midi(1, e);
}

// Counts note-ons and note-offs per pitch; a hanging note is a pitch whose
// counts differ at the end.
static void assert_no_hanging_notes(const RecordingMidiOut& midi, const char* what) {
    int balance[128] = {0};
    for (const auto& m : midi.messages) {
        if (m.type == MIDI_NOTE_ON && m.d2 > 0) balance[m.d1]++;
        else if (m.type == MIDI_NOTE_OFF || (m.type == MIDI_NOTE_ON && m.d2 == 0)) balance[m.d1]--;
    }
    for (int n = 0; n < 128; n++) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s: note %d unbalanced by %d", what, n, balance[n]);
        TEST_ASSERT_EQUAL_MESSAGE(0, balance[n], msg);
    }
}

// One patch shape reused by the four cases below: MIDI in -> node -> MIDI out.
static void modifier_under_held_notes(uint8_t algorithm, uint16_t param, uint8_t value,
                                      const char* what) {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{0x01, 0, 0};
    p.nodes[0] = node(algorithm, 0, 1);
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{0x01, 0, 1};
    TEST_ASSERT_EQUAL_MESSAGE(LOAD_OK, master.load(p), what);
    master.setup();

    uint32_t now = 0;
    note_on(master, 60); note_on(master, 64); note_on(master, 67);
    run_passes(master, 2, now);

    TEST_ASSERT_EQUAL_MESSAGE(PARAM_SET_OK, master.set_node_param(0, param, value), what);
    run_passes(master, 2, now);

    note_off(master, 60); note_off(master, 64); note_off(master, 67);
    run_passes(master, 2, now);
    master.unload();                       // the handover releases anything left

    assert_no_hanging_notes(midi, what);
}

static void test_transpose_offset_moving_under_a_chord_hangs_nothing() {
    modifier_under_held_notes(ALGO_TRANSPOSE, 0, 7, "Transpose offset");
}
static void test_note_quantise_scale_moving_under_a_chord_hangs_nothing() {
    modifier_under_held_notes(ALGO_NOTE_QUANTISE, 0, SCALE_PENTATONIC_MINOR, "NoteQuantise scale");
}
static void test_note_quantise_root_moving_under_a_chord_hangs_nothing() {
    modifier_under_held_notes(ALGO_NOTE_QUANTISE, 1, 7, "NoteQuantise root");
}
static void test_chord_voicing_moving_under_a_chord_hangs_nothing() {
    modifier_under_held_notes(ALGO_CHORD, 0, 3, "Chord voicing");
}
static void test_velocity_curve_moving_under_a_chord_hangs_nothing() {
    modifier_under_held_notes(ALGO_VELOCITY, 0, 2, "VelocityCurve shape");
}

// GateToNote does not hold a ledger: it remembers what it sent instead, and
// this is the test that proves the note number can move under a high gate.
static void test_gate_to_note_releases_the_note_it_sent() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.nodes[0] = node(ALGO_GATE_TO_NOTE, 0, 0);
    p.nodes[0].params[0] = 60;
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{0x01, 0, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    gpio.set_input(0, true);
    run_passes(master, 2, now);
    TEST_ASSERT_EQUAL(PARAM_SET_OK, master.set_node_param(0, 0, 72));   // move it
    run_passes(master, 2, now);
    gpio.set_input(0, false);
    run_passes(master, 2, now);

    assert_no_hanging_notes(midi, "GateToNote note number");
    // Explicitly: the release was for 60, the note that actually sounded.
    bool saw_off_60 = false;
    for (const auto& m : midi.messages) {
        if (m.type == MIDI_NOTE_OFF && m.d1 == 60) saw_off_60 = true;
    }
    TEST_ASSERT_TRUE(saw_off_60);
}

// A note sequencer's root moving mid-pattern is the same rule one level up.
static void test_note_sequencer_root_moving_hangs_nothing() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.gate_ports[0] = GatePortConfig{GATE_PORT_IN, 0};
    p.nodes[0] = node_config(ALGO_NOTE_SEQ);
    p.nodes[0].in_bus[0] = 0;                       // advance from jack 1
    p.nodes[0].out_bus[0] = 0;
    p.nodes[0].params[NoteSequencerBase::P_LENGTH] = 4;
    for (uint8_t s = 0; s < 4; s++) {
        uint8_t* step = &p.nodes[0].params[NoteSequencerBase::STEP_BASE + s * 4];
        step[0] = s;        // degree
        step[1] = 100;      // velocity
        step[2] = 2;        // two edges long, so notes overlap the change
    }
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{0x01, 0, 0};
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    uint32_t now = 0;
    for (int i = 0; i < 6; i++) {
        gpio.set_input(0, true);  run_passes(master, 1, now);
        gpio.set_input(0, false); run_passes(master, 1, now);
        if (i == 2) TEST_ASSERT_EQUAL(PARAM_SET_OK,
                                      master.set_node_param(0, NoteSequencerBase::P_ROOT, 72));
    }
    master.unload();
    assert_no_hanging_notes(midi, "NoteSequencer root");
}

// ---------------------------------------------------------------------------
// No heap allocation on the set_param path, and a no-op set is free.
// ---------------------------------------------------------------------------
static void test_set_param_never_allocates() {
    FakeGpio gpio; RecordingMidiOut midi;
    MixedModeMaster master(gpio, midi);
    Patch p = empty_patch();
    p.nodes[0] = node_config(ALGO_EUCLID_SEQ);
    p.nodes[0].in_bus[0] = 0; p.nodes[0].out_bus[0] = 1;
    p.nodes[0].params[0] = 16;
    p.nodes[1] = node_config(ALGO_POLY_SEQ);
    p.nodes[1].in_bus[0] = 0; p.nodes[1].out_bus[0] = 0;
    p.nodes[2] = node_config(ALGO_DRUM_SEQ_MIDI);
    p.nodes[2].in_bus[0] = 0; p.nodes[2].out_bus[0] = 0;
    p.n_nodes = 3;
    TEST_ASSERT_EQUAL(LOAD_OK, master.load(p));
    master.setup();

    const size_t before = g_allocations;
    for (uint16_t v = 1; v <= 16; v++) master.set_node_param(0, 3, (uint8_t)v);
    for (uint16_t i = 0; i < 200; i++) master.set_node_param(1, (uint16_t)(NoteSequencerBase::STEP_BASE + i), 60);
    for (uint16_t i = 0; i < 200; i++) master.set_node_param(2, (uint16_t)(DrumSeqMidi::VELOCITY_BASE + i), 100);
    TEST_ASSERT_EQUAL(before, g_allocations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_every_parameter_of_every_algorithm_is_described);
    RUN_TEST(test_every_port_and_algorithm_is_named);
    RUN_TEST(test_enum_descriptors_name_every_option);
    RUN_TEST(test_every_parameter_accepts_its_bounds_and_rejects_beyond);
    RUN_TEST(test_unknown_node_is_rejected);
    RUN_TEST(test_euclid_pulses_change_the_pattern_without_moving_the_cursor);
    RUN_TEST(test_shortening_a_sequence_clamps_on_the_next_advance);
    RUN_TEST(test_step_pattern_bytes_round_trip);
    RUN_TEST(test_clock_div_amount_rederives_the_period);
    RUN_TEST(test_clock_div_multiply_from_a_gate_is_still_refused);
    RUN_TEST(test_clock_div_amount_change_lets_the_current_period_complete);
    RUN_TEST(test_out_of_range_parameter_is_rejected_by_the_validator);
    RUN_TEST(test_a_bad_parameter_leaves_the_running_patch_alone);
    RUN_TEST(test_transpose_offset_moving_under_a_chord_hangs_nothing);
    RUN_TEST(test_note_quantise_scale_moving_under_a_chord_hangs_nothing);
    RUN_TEST(test_note_quantise_root_moving_under_a_chord_hangs_nothing);
    RUN_TEST(test_chord_voicing_moving_under_a_chord_hangs_nothing);
    RUN_TEST(test_velocity_curve_moving_under_a_chord_hangs_nothing);
    RUN_TEST(test_gate_to_note_releases_the_note_it_sent);
    RUN_TEST(test_note_sequencer_root_moving_hangs_nothing);
    RUN_TEST(test_set_param_never_allocates);
    return UNITY_END();
}
