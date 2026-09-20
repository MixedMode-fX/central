#include <unity.h>
#include <stdlib.h>
#include <new>

#include "bus/bus_manager.h"
#include "node/patch.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"
#include "clock/musical_division.h"
#include "algorithm/midi/retrigger.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// Retrigger: a held chord is a pad until something re-strikes it. The rhythm
// is the trigger's, the articulation is the length.

static const uint8_t NOTE_IN = 0, NOTE_OUT = 1, GATE_TRIG = 0;

struct Emitted {
    uint32_t subtick;
    bool on;
    uint8_t pitch;
    uint8_t velocity;
    uint8_t channel;
};
// Not `log`: <math.h> is in scope through Unity and owns that name.
static Emitted emitted[256];
static uint16_t logged = 0;

static NodeConfig retrigger_config(uint8_t length, uint8_t feel, uint8_t release, uint8_t velocity){
    NodeConfig c = node_config(ALGO_RETRIGGER);
    c.in_buses[0] = one_bus(NOTE_IN);
    c.in_buses[1] = one_bus(GATE_TRIG);
    c.out_buses[0] = one_bus(NOTE_OUT);
    c.params[0] = length;
    c.params[1] = feel;
    c.params[2] = release;
    c.params[3] = velocity;
    return c;
}

// One pass, in the master's own order: the inlets are published, then
// process(), then tick() with the newest clock count (README, "Every pass").
static void pass(Retrigger& node, BusManager& bus, const MidiEvent* feed,
                 uint32_t subtick, bool trigger = false){
    if (feed) bus.note_write(NOTE_IN, *feed);
    bus.gate_write(GATE_TRIG, trigger);
    bus.swap();
    node.process(bus, subtick * 1000u);
    node.tick(bus, subtick);
    bus.swap();
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        const MidiEvent e = bus.note_read(NOTE_OUT, i);
        if (logged >= 256) break;
        emitted[logged++] = Emitted{subtick, is_note_on(e), e.data1, e.data2, e.channel};
    }
}

// Passes from `sub` up to but not including `to`, one subtick each, with the
// trigger low. Returns where the clock got to.
static uint32_t run_to(Retrigger& node, BusManager& bus, uint32_t sub, uint32_t to){
    for (uint32_t s = sub; s < to; s++) pass(node, bus, nullptr, s);
    return to;
}

static MidiEvent note_on(uint8_t pitch, uint8_t velocity = 100, uint8_t channel = 1){
    return MidiEvent{MIDI_NOTE_ON, channel, pitch, velocity};
}
static MidiEvent note_off(uint8_t pitch, uint8_t channel = 1){
    return MidiEvent{MIDI_NOTE_OFF, channel, pitch, 0};
}

static uint8_t count_on(){
    uint8_t n = 0;
    for (uint16_t i = 0; i < logged; i++) if (emitted[i].on) n++;
    return n;
}
static uint8_t count_off(){
    uint8_t n = 0;
    for (uint16_t i = 0; i < logged; i++) if (!emitted[i].on) n++;
    return n;
}

// Holds C major on the inlet, with no trigger. Returns at subtick 4.
static uint32_t hold_a_triad(Retrigger& node, BusManager& bus){
    const MidiEvent c = note_on(60), e = note_on(64), g = note_on(67);
    pass(node, bus, &c, 0);
    pass(node, bus, &e, 1);
    pass(node, bus, &g, 2);
    return run_to(node, bus, 3, 4);
}

// ---------------------------------------------------------------------------
// The trigger owns the rhythm
// ---------------------------------------------------------------------------

// The point of the node: a chord arriving is not a chord sounding. Anything
// else and the player's timing would be in the figure.
static void test_a_held_chord_sounds_nothing_until_a_trigger() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    hold_a_triad(node, bus);
    run_to(node, bus, 4, 200);

    TEST_ASSERT_EQUAL_UINT16(0, logged);
    TEST_ASSERT_EQUAL_UINT8(3, node.held_count());
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT32(0, node.strikes());
}

static void test_every_trigger_sends_the_whole_chord_again() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    uint32_t sub = hold_a_triad(node, bus);
    for (uint8_t strike = 0; strike < 4; strike++){
        pass(node, bus, nullptr, sub, true);          // the rising edge
        sub = run_to(node, bus, sub + 1, sub + 10);
    }

    // Four strikes of three notes, and a tie means the previous strike is
    // released by the next one rather than by a deadline: 12 note-ons and the
    // 9 note-offs of the three strikes that were replaced.
    TEST_ASSERT_EQUAL_UINT32(4, node.strikes());
    TEST_ASSERT_EQUAL_UINT8(12, count_on());
    TEST_ASSERT_EQUAL_UINT8(9, count_off());
    TEST_ASSERT_EQUAL_UINT8(3, node.sounding_count());
}

// A note-off for the pitch before the note-on that replaces it, or the synth
// downstream hears one long note where two were played.
static void test_the_release_of_a_strike_precedes_the_note_that_replaces_it() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    const MidiEvent c4 = note_on(60);
    pass(node, bus, &c4, 0);
    pass(node, bus, nullptr, 1, true);
    pass(node, bus, nullptr, 2);                      // the trigger falls
    logged = 0;
    pass(node, bus, nullptr, 3, true);

    TEST_ASSERT_EQUAL_UINT16(2, logged);
    TEST_ASSERT_FALSE(emitted[0].on);
    TEST_ASSERT_TRUE(emitted[1].on);
    TEST_ASSERT_EQUAL_UINT8(60, emitted[0].pitch);
    TEST_ASSERT_EQUAL_UINT8(60, emitted[1].pitch);
}

static void test_the_chord_leaves_in_pitch_order() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    // Played top down, so arrival order and pitch order disagree.
    const MidiEvent g = note_on(67), e = note_on(64), c4 = note_on(60);
    pass(node, bus, &g, 0);
    pass(node, bus, &e, 1);
    pass(node, bus, &c4, 2);
    logged = 0;
    pass(node, bus, nullptr, 3, true);

    TEST_ASSERT_EQUAL_UINT16(3, logged);
    TEST_ASSERT_EQUAL_UINT8(60, emitted[0].pitch);
    TEST_ASSERT_EQUAL_UINT8(64, emitted[1].pitch);
    TEST_ASSERT_EQUAL_UINT8(67, emitted[2].pitch);
}

// ---------------------------------------------------------------------------
// The length is a note value
// ---------------------------------------------------------------------------

static void test_a_strike_lasts_its_length_and_not_the_gap() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_LENGTH, 0);
    Retrigger node(c);

    const uint32_t length = division_subticks(DIV_16TH, FEEL_STRAIGHT);
    TEST_ASSERT_EQUAL_UINT32(length, node.length_subticks());

    uint32_t sub = hold_a_triad(node, bus);
    pass(node, bus, nullptr, sub, true);              // strike at `sub`
    const uint32_t struck_at = sub;
    sub = run_to(node, bus, sub + 1, struck_at + length);
    // Still sounding a subtick before the length is up.
    TEST_ASSERT_EQUAL_UINT8(3, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(0, count_off());

    run_to(node, bus, sub, struck_at + length + 2);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(3, count_off());
    // Released, and never played again on its own: the next note-on is the
    // next trigger's.
    TEST_ASSERT_EQUAL_UINT8(3, count_on());
}

static void test_the_feel_lengthens_and_shortens_the_strike() {
    const uint32_t straight = division_subticks(DIV_EIGHTH, FEEL_STRAIGHT);
    const uint32_t dotted = division_subticks(DIV_EIGHTH, FEEL_DOTTED);
    const uint32_t triplet = division_subticks(DIV_EIGHTH, FEEL_TRIPLET);
    TEST_ASSERT_EQUAL_UINT32(straight * 3 / 2, dotted);
    TEST_ASSERT_EQUAL_UINT32(straight * 2 / 3, triplet);

    for (uint8_t feel = FEEL_STRAIGHT; feel <= FEELS; feel++){
        BusManager bus;
        logged = 0;
        NodeConfig c = retrigger_config(DIV_EIGHTH, feel, Retrigger::RT_LENGTH, 0);
        Retrigger node(c);
        const uint32_t length = division_subticks(DIV_EIGHTH, feel);
        TEST_ASSERT_EQUAL_UINT32(length, node.length_subticks());

        const MidiEvent c4 = note_on(60);
        pass(node, bus, &c4, 0);
        pass(node, bus, nullptr, 1, true);
        uint32_t sub = run_to(node, bus, 2, 1 + length);
        TEST_ASSERT_EQUAL_UINT8(1, node.sounding_count());
        run_to(node, bus, sub, 1 + length + 2);
        TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
    }
}

static void test_a_tie_holds_the_chord_through_a_length_that_has_passed() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_64TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    uint32_t sub = hold_a_triad(node, bus);
    pass(node, bus, nullptr, sub, true);
    // Far longer than a 1/64: a tie has no deadline at all.
    run_to(node, bus, sub + 1, sub + 8 * division_subticks(DIV_64TH, FEEL_STRAIGHT));

    TEST_ASSERT_EQUAL_UINT8(3, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(0, count_off());
}

// A running articulation completes; the edit is heard on the next strike.
// This is Metronome's rule for a division change, at note length scope.
static void test_a_length_edit_does_not_cut_the_strike_in_progress() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_QUARTER, FEEL_STRAIGHT, Retrigger::RT_LENGTH, 0);
    Retrigger node(c);

    const uint32_t quarter = division_subticks(DIV_QUARTER, FEEL_STRAIGHT);
    const MidiEvent c4 = note_on(60);
    pass(node, bus, &c4, 0);
    pass(node, bus, nullptr, 1, true);                // a quarter note starts
    uint32_t sub = run_to(node, bus, 2, 40);

    TEST_ASSERT_TRUE(node.set_param(0, DIV_64TH));    // much shorter, now
    sub = run_to(node, bus, sub, quarter);
    TEST_ASSERT_EQUAL_UINT8(1, node.sounding_count()); // the quarter stands
    run_to(node, bus, sub, quarter + 3);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());

    // And the next strike is the new length.
    logged = 0;
    const uint32_t sixty_fourth = division_subticks(DIV_64TH, FEEL_STRAIGHT);
    pass(node, bus, nullptr, quarter + 3, true);
    sub = run_to(node, bus, quarter + 4, quarter + 3 + sixty_fourth);
    TEST_ASSERT_EQUAL_UINT8(1, node.sounding_count());
    run_to(node, bus, sub, quarter + 5 + sixty_fourth);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
}

// ---------------------------------------------------------------------------
// The chord underneath can change
// ---------------------------------------------------------------------------

static void test_a_note_added_to_the_chord_joins_at_the_next_trigger() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    const MidiEvent c4 = note_on(60);
    pass(node, bus, &c4, 0);
    pass(node, bus, nullptr, 1, true);
    TEST_ASSERT_EQUAL_UINT8(1, count_on());

    logged = 0;
    const MidiEvent e4 = note_on(64);
    pass(node, bus, &e4, 2);
    run_to(node, bus, 3, 20);
    // Heard nothing: the new note waits for the edge.
    TEST_ASSERT_EQUAL_UINT16(0, logged);
    TEST_ASSERT_EQUAL_UINT8(2, node.held_count());

    pass(node, bus, nullptr, 20, true);
    TEST_ASSERT_EQUAL_UINT8(2, count_on());
    TEST_ASSERT_EQUAL_UINT8(2, node.sounding_count());
}

static void test_a_note_off_stops_that_note_and_leaves_the_others() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    uint32_t sub = hold_a_triad(node, bus);
    pass(node, bus, nullptr, sub, true);
    logged = 0;

    const MidiEvent lift = note_off(64);
    pass(node, bus, &lift, sub + 1);
    TEST_ASSERT_EQUAL_UINT16(1, logged);
    TEST_ASSERT_FALSE(emitted[0].on);
    TEST_ASSERT_EQUAL_UINT8(64, emitted[0].pitch);
    TEST_ASSERT_EQUAL_UINT8(2, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(2, node.held_count());

    // And it is not in the next strike either.
    logged = 0;
    pass(node, bus, nullptr, sub + 2, true);
    TEST_ASSERT_EQUAL_UINT8(2, count_on());
    for (uint16_t i = 0; i < logged; i++) TEST_ASSERT_TRUE(emitted[i].pitch != 64 || !emitted[i].on);
}

static void test_lifting_the_chord_leaves_nothing_sounding_and_nothing_to_strike() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    uint32_t sub = hold_a_triad(node, bus);
    pass(node, bus, nullptr, sub, true);
    const MidiEvent l1 = note_off(60), l2 = note_off(64), l3 = note_off(67);
    pass(node, bus, &l1, sub + 1);
    pass(node, bus, &l2, sub + 2);
    pass(node, bus, &l3, sub + 3);

    TEST_ASSERT_EQUAL_UINT8(0, node.held_count());
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(3, count_on());
    TEST_ASSERT_EQUAL_UINT8(3, count_off());

    // A trigger over an empty chord is a rest, not a strike.
    logged = 0;
    const uint32_t before = node.strikes();
    pass(node, bus, nullptr, sub + 4, true);
    TEST_ASSERT_EQUAL_UINT16(0, logged);
    TEST_ASSERT_EQUAL_UINT32(before, node.strikes());
}

static void test_velocity_keeps_what_was_played_or_replaces_it() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    const MidiEvent soft = note_on(60, 30, 3), loud = note_on(64, 120, 3);
    pass(node, bus, &soft, 0);
    pass(node, bus, &loud, 1);
    pass(node, bus, nullptr, 2, true);

    TEST_ASSERT_EQUAL_UINT16(2, logged);
    TEST_ASSERT_EQUAL_UINT8(30, emitted[0].velocity);
    TEST_ASSERT_EQUAL_UINT8(120, emitted[1].velocity);
    TEST_ASSERT_EQUAL_UINT8(3, emitted[0].channel);     // the channel is the player's too

    logged = 0;
    TEST_ASSERT_TRUE(node.set_param(3, 90));
    pass(node, bus, nullptr, 3);
    pass(node, bus, nullptr, 4, true);
    for (uint16_t i = 0; i < logged; i++){
        if (emitted[i].on) TEST_ASSERT_EQUAL_UINT8(90, emitted[i].velocity);
    }
}

// ---------------------------------------------------------------------------
// Nothing hangs
// ---------------------------------------------------------------------------

static void test_a_patch_swap_releases_the_strike() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    uint32_t sub = hold_a_triad(node, bus);
    pass(node, bus, nullptr, sub, true);
    logged = 0;

    bus.swap();
    node.silence(bus);
    bus.swap();
    uint8_t offs = 0;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        if (is_note_off(bus.note_read(NOTE_OUT, i))) offs++;
    }
    TEST_ASSERT_EQUAL_UINT8(3, offs);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
}

// A tie waits for an edge and a length waits for a subtick, and a stopped
// clock has neither. The chord is still held on the inlet, so its own
// note-offs are not coming.
static void test_a_transport_stop_releases_the_strike() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_BAR, FEEL_STRAIGHT, Retrigger::RT_LENGTH, 0);
    Retrigger node(c);

    uint32_t sub = hold_a_triad(node, bus);
    pass(node, bus, nullptr, sub, true);
    TEST_ASSERT_EQUAL_UINT8(3, node.sounding_count());

    bus.swap();
    node.transport_stopped(bus);
    node.transport_stopped(bus);         // told twice: the second writes nothing
    bus.swap();
    uint8_t offs = 0;
    for (uint8_t i = 0; i < bus.note_count(NOTE_OUT); i++){
        if (is_note_off(bus.note_read(NOTE_OUT, i))) offs++;
    }
    TEST_ASSERT_EQUAL_UINT8(3, offs);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
    TEST_ASSERT_EQUAL_UINT8(3, node.held_count());      // the chord is still held

    // And the next trigger plays it again.
    logged = 0;
    pass(node, bus, nullptr, sub + 1);
    pass(node, bus, nullptr, sub + 2, true);
    TEST_ASSERT_EQUAL_UINT8(3, count_on());
}

static void test_a_controller_message_travels_straight_through() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_16TH, FEEL_STRAIGHT, Retrigger::RT_TIE, 0);
    Retrigger node(c);

    const MidiEvent cc = {MIDI_CONTROL_CHANGE, 1, 74, 64};
    bus.note_write(NOTE_IN, cc);
    bus.swap();
    node.process(bus, 0);
    bus.swap();

    TEST_ASSERT_EQUAL_UINT8(1, bus.note_count(NOTE_OUT));
    const MidiEvent out = bus.note_read(NOTE_OUT, 0);
    TEST_ASSERT_EQUAL_UINT8(MIDI_CONTROL_CHANGE, out.type);
    TEST_ASSERT_EQUAL_UINT8(74, out.data1);
    TEST_ASSERT_EQUAL_UINT8(0, node.sounding_count());
}

static void test_the_retrigger_never_allocates() {
    BusManager bus;
    logged = 0;
    NodeConfig c = retrigger_config(DIV_32ND, FEEL_TRIPLET, Retrigger::RT_LENGTH, 0);
    Retrigger node(c);
    const size_t before = g_allocations;

    uint32_t sub = 0;
    for (uint8_t round = 0; round < 40; round++){
        const MidiEvent on = note_on((uint8_t)(48 + (round % 24)), 90);
        pass(node, bus, &on, sub++);
        pass(node, bus, nullptr, sub++, true);
        sub = run_to(node, bus, sub, sub + 12);
        const MidiEvent off = note_off((uint8_t)(48 + (round % 24)));
        pass(node, bus, &off, sub++);
        logged = 0;
    }
    TEST_ASSERT_EQUAL_size_t(before, g_allocations);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_held_chord_sounds_nothing_until_a_trigger);
    RUN_TEST(test_every_trigger_sends_the_whole_chord_again);
    RUN_TEST(test_the_release_of_a_strike_precedes_the_note_that_replaces_it);
    RUN_TEST(test_the_chord_leaves_in_pitch_order);
    RUN_TEST(test_a_strike_lasts_its_length_and_not_the_gap);
    RUN_TEST(test_the_feel_lengthens_and_shortens_the_strike);
    RUN_TEST(test_a_tie_holds_the_chord_through_a_length_that_has_passed);
    RUN_TEST(test_a_length_edit_does_not_cut_the_strike_in_progress);
    RUN_TEST(test_a_note_added_to_the_chord_joins_at_the_next_trigger);
    RUN_TEST(test_a_note_off_stops_that_note_and_leaves_the_others);
    RUN_TEST(test_lifting_the_chord_leaves_nothing_sounding_and_nothing_to_strike);
    RUN_TEST(test_velocity_keeps_what_was_played_or_replaces_it);
    RUN_TEST(test_a_patch_swap_releases_the_strike);
    RUN_TEST(test_a_transport_stop_releases_the_strike);
    RUN_TEST(test_a_controller_message_travels_straight_through);
    RUN_TEST(test_the_retrigger_never_allocates);
    return UNITY_END();
}
