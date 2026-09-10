#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/global_scale.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "algorithm/midi/note_delay.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() { global_scale::set(SCALE_MAJOR, 0); }
void tearDown() { global_scale::set(SCALE_CHROMATIC, 0); }

// NoteDelay (#35): every edge in a patch descends from one clock, so a patch
// had exactly one rhythmic surface and nothing could leave it on purpose.

static const uint8_t NOTE_IN = 0, NOTE_OUT = 1, GATE_CLEAR = 0;

// What came out, and when.
struct Emitted {
    uint32_t at;
    bool on;
    uint8_t pitch;
    uint8_t velocity;
    uint8_t channel;
};
// Not `log`: <math.h> is in scope through Unity and owns that name.
static Emitted emitted[256];
static uint16_t logged = 0;

static NodeConfig delay_config(uint8_t sync, uint8_t time_tens, uint8_t repeats,
                               uint8_t interval, uint8_t decay){
    NodeConfig c = node_config(ALGO_NOTE_DELAY);
    c.in_bus[0] = NOTE_IN;
    c.in_bus[1] = GATE_CLEAR;
    c.out_bus[0] = NOTE_OUT;
    c.params[0] = sync;
    c.params[3] = time_tens;
    c.params[4] = repeats;
    c.params[5] = interval;
    c.params[6] = decay;
    c.params[11] = NoteDelay::ND_MUTE;      // the repeats alone, so the log is only echoes
    return c;
}

// One pass, optionally feeding one event in, recording everything that came
// out. `subtick` is delivered too, so the synced mode has a clock.
static void pass(NoteDelay& node, BusManager& bus, uint32_t now_us,
                 const MidiEvent* feed, uint32_t subtick, bool clear = false){
    if (feed) bus.note_write(NOTE_IN, *feed);
    bus.gate_write(GATE_CLEAR, clear);
    bus.swap();
    node.tick(bus, subtick);
    node.process(bus, now_us);
    bus.swap();
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        const MidiEvent e = bus.note_read(NOTE_OUT, i);
        if (logged >= 256) break;
        emitted[logged++] = Emitted{now_us, is_note_on(e), e.data1, e.data2, e.channel};
    }
}

// Runs `ms` milliseconds of passes at 1 ms each from `from_us`, with the
// master clock running at `subticks_per_ms`.
static uint32_t run(NoteDelay& node, BusManager& bus, uint32_t from_us, uint32_t ms,
                    uint32_t subticks_per_ms = 0){
    uint32_t t = from_us;
    for (uint32_t i = 0; i < ms; i++){
        pass(node, bus, t, nullptr, (t / 1000u) * subticks_per_ms);
        t += 1000;
    }
    return t;
}

static uint8_t count_on(){
    uint8_t n = 0;
    for (uint16_t i = 0; i < logged; i++) if (emitted[i].on) n++;
    return n;
}

// ---------------------------------------------------------------------------
// The delay
// ---------------------------------------------------------------------------

static void test_a_note_comes_back_once_per_repeat_on_time() {
    BusManager bus;
    logged = 0;
    // 100 ms, three repeats, no transposition, no decay.
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 3, 0, 100);
    NoteDelay node(c);

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    run(node, bus, 1000, 400);

    TEST_ASSERT_EQUAL_UINT8(3, count_on());
    uint32_t expect = 100000;
    for (uint16_t i = 0; i < logged; i++){
        if (!emitted[i].on) continue;
        TEST_ASSERT_EQUAL_UINT8(60, emitted[i].pitch);
        TEST_ASSERT_EQUAL_UINT32(expect, emitted[i].at);
        expect += 100000;
    }
}

static void test_an_echo_is_held_as_long_as_the_note_was() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 2, 0, 100);
    NoteDelay node(c);

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    const MidiEvent off = {MIDI_NOTE_OFF, 1, 60, 0};
    pass(node, bus, 0, &on, 0);
    uint32_t t = run(node, bus, 1000, 39);          // held for 40 ms
    pass(node, bus, t, &off, 0);
    run(node, bus, t + 1000, 400);

    // Two note-ons and two note-offs, and each pair is 40 ms apart - the
    // articulation is the input's, not a parameter.
    TEST_ASSERT_EQUAL_UINT16(4, logged);
    TEST_ASSERT_TRUE(emitted[0].on && !emitted[1].on && emitted[2].on && !emitted[3].on);
    TEST_ASSERT_EQUAL_UINT32(100000, emitted[0].at);
    TEST_ASSERT_EQUAL_UINT32(140000, emitted[1].at);
    TEST_ASSERT_EQUAL_UINT32(200000, emitted[2].at);
    TEST_ASSERT_EQUAL_UINT32(240000, emitted[3].at);
}

static void test_decay_quietens_each_repeat_and_stops_when_it_runs_out() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 5, 8, 0, 50);
    NoteDelay node(c);

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    run(node, bus, 1000, 1000);

    // 100 halves to 50, 25, 12, 6, 3, 1, and then to nothing - so seven
    // repeats sound and the eighth is silence rather than a note-off shaped
    // like a note-on.
    const uint8_t expect[7] = {50, 25, 12, 6, 3, 1, 0};
    uint8_t n = 0;
    for (uint16_t i = 0; i < logged; i++){
        if (!emitted[i].on) continue;
        TEST_ASSERT_TRUE(n < 7);
        TEST_ASSERT_EQUAL_UINT8(expect[n], emitted[i].velocity);
        n++;
    }
    TEST_ASSERT_EQUAL_UINT8(6, n);
}

// ---------------------------------------------------------------------------
// The canon
// ---------------------------------------------------------------------------

static void test_the_interval_is_scale_steps_so_a_canon_stays_in_key() {
    BusManager bus;
    logged = 0;
    // A canon at the third: two scale steps per repeat, in C major.
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 3, 2, 100);
    NoteDelay node(c);

    // C4 -> E4 (four semitones), G4 (seven), B4 (eleven). Steps of the scale,
    // not a fixed stack of semitones: the third above C is major and the one
    // above E is minor, and the node never had to know that.
    TEST_ASSERT_EQUAL_UINT8(64, node.pitch_for(60, 1));
    TEST_ASSERT_EQUAL_UINT8(67, node.pitch_for(60, 2));
    TEST_ASSERT_EQUAL_UINT8(71, node.pitch_for(60, 3));
    // And from D4 the same two steps are a *minor* third: F4, A4, C5.
    TEST_ASSERT_EQUAL_UINT8(65, node.pitch_for(62, 1));
    TEST_ASSERT_EQUAL_UINT8(69, node.pitch_for(62, 2));
    TEST_ASSERT_EQUAL_UINT8(72, node.pitch_for(62, 3));

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    run(node, bus, 1000, 400);
    const uint8_t expect[3] = {64, 67, 71};
    uint8_t n = 0;
    for (uint16_t i = 0; i < logged; i++){
        if (!emitted[i].on) continue;
        TEST_ASSERT_EQUAL_UINT8(expect[n++], emitted[i].pitch);
    }
    TEST_ASSERT_EQUAL_UINT8(3, n);
}

static void test_no_interval_is_transparent_to_a_chromatic_line() {
    BusManager bus;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 3, 0, 100);
    NoteDelay node(c);
    // C# is not in C major, and with nothing to transpose by it is not
    // quantised on the way through either: a delay that is not transposing
    // has no business moving a pitch.
    for (uint8_t p = 55; p < 70; p++){
        for (uint8_t k = 1; k <= 3; k++) TEST_ASSERT_EQUAL_UINT8(p, node.pitch_for(p, k));
    }
}

static void test_a_note_outside_the_key_echoes_inside_it() {
    BusManager bus;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 2, 1, 100);
    NoteDelay node(c);
    // C#4 is not in C major. It resolves to the degree of the note above it -
    // D, which is the direction semitone_to_scale_degree leans and the
    // direction scale_quantise breaks a tie - and the repeats step from
    // there, so they are in the key even though the note was not.
    TEST_ASSERT_EQUAL_UINT8(64, node.pitch_for(61, 1));      // E
    TEST_ASSERT_EQUAL_UINT8(65, node.pitch_for(61, 2));      // F
}

static void test_an_echo_off_the_end_of_the_keyboard_is_a_rest() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 4, 20, 100);
    NoteDelay node(c);
    const MidiEvent on = {MIDI_NOTE_ON, 1, 120, 100};
    pass(node, bus, 0, &on, 0);
    run(node, bus, 1000, 600);
    // Twenty scale steps above a high note leaves MIDI before the fourth
    // repeat; those are dropped rather than folded, and nothing is left owing
    // a note-off.
    TEST_ASSERT_TRUE(count_on() < 4);
    TEST_ASSERT_EQUAL_UINT8(0, node.scheduled());
}

// ---------------------------------------------------------------------------
// Leaving the grid
// ---------------------------------------------------------------------------

static void test_spread_makes_each_gap_longer_than_the_last() {
    BusManager bus;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 5, 0, 100);
    c.params[8] = 25;                           // +25% per gap
    NoteDelay node(c);

    // At zero this would be 100, 200, 300, 400, 500 ms - one grid. With
    // spread the gaps grow, so the repeats stop lining up with anything.
    uint32_t previous_gap = 0;
    uint32_t previous = 0;
    for (uint8_t k = 1; k <= 5; k++){
        const uint32_t at = node.delay_of(k);
        const uint32_t gap = at - previous;
        TEST_ASSERT_TRUE_MESSAGE(gap > previous_gap, "a gap that did not grow");
        previous_gap = gap;
        previous = at;
    }
    TEST_ASSERT_EQUAL_UINT32(100000, node.delay_of(1));
    TEST_ASSERT_EQUAL_UINT32(225000, node.delay_of(2));      // 200 + 25% of one period
    TEST_ASSERT_EQUAL_UINT32(375000, node.delay_of(3));      // 300 + 75%
}

static void test_a_negative_spread_makes_the_echoes_accelerate() {
    BusManager bus;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 6, 0, 100);
    c.params[8] = (uint8_t)(int8_t)-20;
    NoteDelay node(c);

    uint32_t previous_gap = 0xFFFFFFFFu;
    uint32_t previous = 0;
    for (uint8_t k = 1; k <= 4; k++){
        const uint32_t at = node.delay_of(k);
        const uint32_t gap = at - previous;
        TEST_ASSERT_TRUE_MESSAGE(gap < previous_gap, "a gap that did not shrink");
        previous_gap = gap;
        previous = at;
    }
    // However hard it accelerates, a repeat never arrives before the note
    // that caused it.
    for (uint8_t k = 1; k <= 6; k++) TEST_ASSERT_TRUE(node.delay_of(k) >= 1);
}

static void test_a_synced_delay_counts_subticks_and_not_milliseconds() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_CLOCK, 0, 2, 0, 100);
    c.params[1] = DIV_16TH;
    NoteDelay node(c);

    // A sixteenth is CLOCK_SUBTICKS_PER_QUARTER / 4 subticks, whatever the
    // tempo is doing, so the delay is exact and cannot drift against a
    // Metronome at the same division.
    const uint32_t sixteenth = CLOCK_SUBTICKS_PER_QUARTER / 4;
    TEST_ASSERT_EQUAL_UINT32(sixteenth, node.delay_of(1));
    TEST_ASSERT_EQUAL_UINT32(sixteenth * 2, node.delay_of(2));

    // Run the clock at six subticks a millisecond and check the echoes land
    // on the subtick rather than on the clock time.
    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    run(node, bus, 1000, 200, 6);
    TEST_ASSERT_EQUAL_UINT8(2, count_on());
    TEST_ASSERT_EQUAL_UINT32((sixteenth / 6) * 1000u, emitted[0].at);
}

// ---------------------------------------------------------------------------
// Nothing hangs
// ---------------------------------------------------------------------------

static void test_the_scale_moving_between_the_on_and_the_off_strands_nothing() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 2, 2, 100);
    NoteDelay node(c);

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    const MidiEvent off = {MIDI_NOTE_OFF, 1, 60, 0};
    pass(node, bus, 0, &on, 0);
    uint32_t t = run(node, bus, 1000, 150);      // the first echo has sounded

    // The whole key moves under it. What was sent is what is released,
    // because the pitch was decided when the echo was scheduled.
    global_scale::set(SCALE_NATURAL_MINOR, 9);
    pass(node, bus, t, &off, 0);
    run(node, bus, t + 1000, 400);

    uint8_t ons = 0, offs = 0;
    uint8_t on_pitch[8], off_pitch[8];
    for (uint16_t i = 0; i < logged; i++){
        if (emitted[i].on) on_pitch[ons++] = emitted[i].pitch;
        else off_pitch[offs++] = emitted[i].pitch;
    }
    TEST_ASSERT_EQUAL_UINT8(2, ons);
    TEST_ASSERT_EQUAL_UINT8(2, offs);
    TEST_ASSERT_EQUAL_UINT8(on_pitch[0], off_pitch[0]);
    TEST_ASSERT_EQUAL_UINT8(on_pitch[1], off_pitch[1]);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
}

static void test_clear_drops_what_is_waiting_and_releases_what_is_sounding() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 4, 0, 100);
    NoteDelay node(c);

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    uint32_t t = run(node, bus, 1000, 150);
    TEST_ASSERT_EQUAL_UINT8(1, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(3, node.scheduled());

    logged = 0;
    pass(node, bus, t, nullptr, 0, true);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(0, node.scheduled());
    TEST_ASSERT_EQUAL_UINT16(1, logged);
    TEST_ASSERT_FALSE(emitted[0].on);
    TEST_ASSERT_EQUAL_UINT8(60, emitted[0].pitch);

    // And nothing arrives afterwards from the repeats that were dropped.
    logged = 0;
    run(node, bus, t + 1000, 500);
    TEST_ASSERT_EQUAL_UINT16(0, logged);
}

static void test_a_patch_swap_releases_every_echo() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 2, 4, 0, 100);
    NoteDelay node(c);
    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    run(node, bus, 1000, 120);
    TEST_ASSERT_TRUE(node.sounding_count() > 1);

    node.silence(bus);
    bus.swap();
    uint8_t offs = 0;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        TEST_ASSERT_TRUE(is_note_off(bus.note_read(NOTE_OUT, i)));
        offs++;
    }
    TEST_ASSERT_TRUE(offs >= 2);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
}

static void test_an_echo_that_cannot_be_released_is_never_emitted() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 20, 8, 0, 100);
    NoteDelay node(c);

    // Eight repeats each of six notes is forty-eight echoes against a table
    // of MAX_ECHOES. The overflow is dropped and counted, never emitted, so
    // the count that comes out can never exceed what can be released.
    uint32_t t = 0;
    for (uint8_t p = 60; p < 66; p++){
        const MidiEvent on = {MIDI_NOTE_ON, 1, p, 100};
        pass(node, bus, t, &on, 0);
        t += 1000;
    }
    TEST_ASSERT_TRUE_MESSAGE(node.dropped() > 0, "the table did not overflow");
    TEST_ASSERT_EQUAL_UINT8(NoteDelay::MAX_ECHOES, node.scheduled());

    run(node, bus, t, 3000);
    TEST_ASSERT_EQUAL_UINT8(NoteDelay::MAX_ECHOES, count_on());
}

static void test_dry_pass_sends_the_input_through_and_mute_does_not() {
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 10, 1, 0, 100);
    c.params[11] = NoteDelay::ND_PASS;
    NoteDelay node(c);

    const MidiEvent on = {MIDI_NOTE_ON, 1, 60, 100};
    pass(node, bus, 0, &on, 0);
    TEST_ASSERT_EQUAL_UINT16(1, logged);
    TEST_ASSERT_TRUE(emitted[0].on);
    TEST_ASSERT_EQUAL_UINT32(0, emitted[0].at);              // now, not in 100 ms

    // A controller message is passed through and never echoed: a delay of a
    // CC is not a musical idea.
    logged = 0;
    const MidiEvent cc = {MIDI_CONTROL_CHANGE, 1, 74, 64};
    pass(node, bus, 1000, &cc, 0);
    run(node, bus, 2000, 400);
    uint8_t notes = 0;
    for (uint16_t i = 0; i < logged; i++) if (emitted[i].pitch == 74 && emitted[i].on) notes++;
    TEST_ASSERT_EQUAL_UINT8(0, notes);
}

static void test_the_delay_never_allocates() {
    const size_t before = g_allocations;
    BusManager bus;
    logged = 0;
    NodeConfig c = delay_config(NoteDelay::ND_FREE, 5, 6, 2, 80);
    NoteDelay node(c);
    uint32_t t = 0;
    for (uint8_t round = 0; round < 20; round++){
        const MidiEvent on = {MIDI_NOTE_ON, 1, (uint8_t)(60 + (round % 5)), 100};
        pass(node, bus, t, &on, 0);
        t = run(node, bus, t + 1000, 30);
        const MidiEvent off = {MIDI_NOTE_OFF, 1, (uint8_t)(60 + (round % 5)), 0};
        pass(node, bus, t, &off, 0);
        t = run(node, bus, t + 1000, 30);
        logged = 0;
    }
    TEST_ASSERT_EQUAL_size_t(before, g_allocations);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_note_comes_back_once_per_repeat_on_time);
    RUN_TEST(test_an_echo_is_held_as_long_as_the_note_was);
    RUN_TEST(test_decay_quietens_each_repeat_and_stops_when_it_runs_out);
    RUN_TEST(test_the_interval_is_scale_steps_so_a_canon_stays_in_key);
    RUN_TEST(test_no_interval_is_transparent_to_a_chromatic_line);
    RUN_TEST(test_a_note_outside_the_key_echoes_inside_it);
    RUN_TEST(test_an_echo_off_the_end_of_the_keyboard_is_a_rest);
    RUN_TEST(test_spread_makes_each_gap_longer_than_the_last);
    RUN_TEST(test_a_negative_spread_makes_the_echoes_accelerate);
    RUN_TEST(test_a_synced_delay_counts_subticks_and_not_milliseconds);
    RUN_TEST(test_the_scale_moving_between_the_on_and_the_off_strands_nothing);
    RUN_TEST(test_clear_drops_what_is_waiting_and_releases_what_is_sounding);
    RUN_TEST(test_a_patch_swap_releases_every_echo);
    RUN_TEST(test_an_echo_that_cannot_be_released_is_never_emitted);
    RUN_TEST(test_dry_pass_sends_the_input_through_and_mute_does_not);
    RUN_TEST(test_the_delay_never_allocates);
    return UNITY_END();
}
