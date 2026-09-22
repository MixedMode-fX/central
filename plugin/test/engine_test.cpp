// The engine, driven the way a host drives it: blocks of samples with
// messages at sample offsets, a playhead, a window sending SysEx, and a
// project saving and restoring the state. No plugin framework: this is the
// part of the plugin a compiler on any desk can run.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <new>

#include "engine.h"
#include "hal/midi_types.h"
#include "node/registry.h"
#include "patch/default_patch.h"
#include "protocol/sysex.h"

using namespace mmmc_plugin;

// Every operator new is counted, so the tests can say the engine allocates
// nothing once it is running - the firmware's rule, kept in the plugin.
static size_t g_allocations = 0;
void* operator new(size_t size){ g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size){ g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)){ g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { const long long va = (long long)(a); const long long vb = (long long)(b); if (va != vb){ g_failures++; fprintf(stderr, "%s:%d: %s == %lld, expected %s == %lld\n", __FILE__, __LINE__, #a, va, #b, vb); } } while (0)

static constexpr double RATE = 48000.0;

// An Engine is large, and there is one per plugin instance; the tests keep
// two, statically, as a host keeps its own on the heap.
static Engine engine_a;
static Engine engine_b;

static Engine& fresh(Engine& e){
    e.~Engine();
    new (&e) Engine();
    e.boot(0x1234u, 0x5678u);
    e.set_sample_rate(RATE);
    return e;
}

static InEvent channel_event(uint32_t sample, uint8_t source, uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2){
    InEvent e; e.sample = sample; e.source = source; e.type = type; e.channel = channel; e.d1 = d1; e.d2 = d2;
    return e;
}

// MidiIn on the host's cable -> note bus 0 -> MidiOut, on a cable the plugin
// has no wire for: the mask is the module's, and the DAW gets it anyway.
static void load_thru(Engine& e){
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{HOST_IN, 0, one_bus(0)};
    p.midi_out[0] = MidiOutConfig{mmMIDI_SERIAL_2, 0, one_bus(0)};
    CHECK_EQ(e.patches().apply(p, e.patches().globals(), e.now()), APPLY_OK);
}

static std::vector<uint8_t> framed(uint8_t command, std::vector<uint8_t> args = {}){
    std::vector<uint8_t> m = {0xF0, SYSEX_MANUFACTURER, SYSEX_DEFAULT_DEVICE, command, SYSEX_PROTOCOL_VERSION};
    for (uint8_t b : args) m.push_back(b);
    m.push_back(0xF7);
    return m;
}

// Runs `n` samples of silence.
static void run_silent(Engine& e, uint32_t n, uint32_t block = 480){
    while (n > 0){
        const uint32_t take = n < block ? n : block;
        e.process(take, nullptr, 0);
        n -= take;
    }
}

// --- boot -------------------------------------------------------------------

static void test_boots_on_the_default_patch_following_the_host(){
    Engine& e = fresh(engine_a);
    CHECK(e.patches().running_defaults());
    CHECK_EQ(e.master().clock().source(), MasterClock::CLOCK_MIDI);
    CHECK_EQ(e.master().node_count(), 0);
}

// --- passes on the sample clock -------------------------------------------

static void test_a_block_runs_the_passes_it_covers(){
    Engine& e = fresh(engine_a);
    CHECK_EQ(e.now(), 0);
    e.process(480, nullptr, 0);            // 10 ms at 48 kHz: passes at 0, 48, ... 432
    CHECK_EQ(e.now(), 10 * PASS_US);
    e.process(24, nullptr, 0);             // the pass at 480 is this block's first sample
    CHECK_EQ(e.now(), 11 * PASS_US);
    e.process(24, nullptr, 0);             // 504..527 holds no pass: 528 is the next block's
    CHECK_EQ(e.now(), 11 * PASS_US);
}

static void test_a_message_is_heard_by_the_pass_that_covers_its_sample(){
    Engine& e = fresh(engine_a);
    load_thru(e);
    const InEvent in[] = {
        channel_event(100, HOST_IN, MIDI_NOTE_ON, 1, 60, 100),
        channel_event(300, HOST_IN, MIDI_NOTE_OFF, 1, 60, 0),
    };
    e.process(480, in, 2);
    const PluginMidiOut& out = e.midi();
    CHECK_EQ(out.n_events, 2);
    if (out.n_events == 2){
        CHECK_EQ(out.events[0].status, 0x90);
        CHECK_EQ(out.events[0].d1, 60);
        CHECK_EQ(out.events[0].d2, 100);
        CHECK_EQ(out.events[0].length, 3);
        // Passes at 0, 48, 96, 144, ...: the note-on at sample 100 is heard
        // at 144 and leaves in that pass, never before it arrived.
        CHECK(out.events[0].sample >= 100);
        CHECK(out.events[0].sample < 100 + 2 * 48);
        CHECK_EQ(out.events[1].status, 0x80);
        CHECK(out.events[1].sample >= 300);
        CHECK(out.events[1].sample < 300 + 2 * 48);
    }
}

static void test_a_message_after_the_last_pass_waits_for_the_next_block(){
    Engine& e = fresh(engine_a);
    load_thru(e);
    const InEvent in[] = { channel_event(470, HOST_IN, MIDI_NOTE_ON, 1, 64, 90) };
    e.process(480, in, 1);
    CHECK_EQ(e.midi().n_events, 0);
    e.process(480, nullptr, 0);
    CHECK_EQ(e.midi().n_events, 1);
    if (e.midi().n_events == 1) CHECK_EQ(e.midi().events[0].sample, 0);
}

static void test_a_message_from_the_window_takes_the_next_pass(){
    Engine& e = fresh(engine_a);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{mmMIDI_USB_1, 0, one_bus(0)};
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, one_bus(0)};
    CHECK_EQ(e.patches().apply(p, e.patches().globals(), e.now()), APPLY_OK);
    e.receive(mmMIDI_USB_1, MIDI_NOTE_ON, 3, 48, 80);
    e.process(48, nullptr, 0);
    CHECK_EQ(e.midi().n_events, 1);
    if (e.midi().n_events == 1){
        CHECK_EQ(e.midi().events[0].status, 0x92);
        CHECK_EQ(e.midi().events[0].sample, 0);
    }
}

static void test_program_change_is_two_bytes_and_realtime_one(){
    Engine& e = fresh(engine_a);
    load_thru(e);
    const InEvent in[] = { channel_event(0, HOST_IN, MIDI_PROGRAM_CHANGE, 2, 5, 0) };
    e.process(48, in, 1);
    CHECK_EQ(e.midi().n_events, 1);
    if (e.midi().n_events == 1){
        CHECK_EQ(e.midi().events[0].status, 0xC1);
        CHECK_EQ(e.midi().events[0].length, 2);
    }
    // The clock, out of the module: a musical mask reaches the host.
    GlobalSettings g = e.patches().globals();
    g.clock_source = MasterClock::CLOCK_INTERNAL;
    g.clock_out_mask = mmMIDI_USB_2;
    e.patches().set_globals(g, e.now());
    e.master().clock().start();
    run_silent(e, 24000);                  // half a second at 120 BPM: a beat
    uint32_t clocks = 0;
    // Only the last block is in the buffer; count across a fresh beat.
    for (uint32_t i = 0; i < 50; i++){
        e.process(480, nullptr, 0);
        for (uint32_t k = 0; k < e.midi().n_events; k++){
            if (e.midi().events[k].status == MIDI_CLOCK){ clocks++; CHECK_EQ(e.midi().events[k].length, 1); }
        }
    }
    CHECK(clocks >= MASTER_PPQN - 1 && clocks <= MASTER_PPQN + 1);
}

// --- the host as a MIDI clock ---------------------------------------------

static void test_host_clock_starts_and_pulses_on_the_grid(){
    HostClock clock;
    InEvent out[128];
    // One second from the top at 120 BPM: a start, then every 1/24 quarter
    // after it - 47 of them before two beats are up.
    uint32_t n = clock.block(true, 0.0, 120.0, RATE, 48000, out, 128);
    CHECK_EQ(n, 48);
    if (n == 48){
        CHECK_EQ(out[0].type, MIDI_START);
        CHECK_EQ(out[0].sample, 0);
        CHECK_EQ(out[1].type, MIDI_CLOCK);
        CHECK_EQ(out[1].sample, 1000);       // 1/24 of a beat at 120 BPM, 48 kHz
        CHECK_EQ(out[47].sample, 47000);
        CHECK_EQ(out[1].source, HOST_IN);
    }
    // The next second: pulses 48..95, the first on the block's first sample.
    n = clock.block(true, 2.0, 120.0, RATE, 48000, out, 128);
    CHECK_EQ(n, 48);
    if (n == 48){
        CHECK_EQ(out[0].type, MIDI_CLOCK);
        CHECK_EQ(out[0].sample, 0);
        CHECK_EQ(out[47].sample, 47000);
    }
    // Stopping is a stop, once.
    n = clock.block(false, 4.0, 120.0, RATE, 480, out, 128);
    CHECK_EQ(n, 1);
    if (n == 1) CHECK_EQ(out[0].type, MIDI_STOP);
    n = clock.block(false, 4.0, 120.0, RATE, 480, out, 128);
    CHECK_EQ(n, 0);
}

static void test_host_clock_rephases_on_a_loop_without_a_start(){
    HostClock clock;
    InEvent out[128];
    CHECK_EQ(clock.block(true, 3.5, 120.0, RATE, 48000, out, 128), 48);   // start + pulses 85..131 over 2 beats
    // Looped back to the top: the pulses are on the new grid, no start.
    const uint32_t n = clock.block(true, 0.0, 120.0, RATE, 4800, out, 128);
    CHECK_EQ(n, 4);                        // 0.2 beats: pulses 1..4
    if (n == 4){
        CHECK_EQ(out[0].type, MIDI_CLOCK);
        CHECK_EQ(out[0].sample, 1000);
    }
    // No tempo from the host: the transport still moves, no pulses.
    HostClock mute;
    CHECK_EQ(mute.block(true, 0.0, 0.0, RATE, 4800, out, 128), 1);
    CHECK_EQ(mute.block(true, 0.1, 0.0, RATE, 4800, out, 128), 0);
}

static void test_the_module_follows_the_host_transport(){
    Engine& e = fresh(engine_a);
    HostClock clock;
    InEvent in[256];
    // Half a second of play from the top at 120 BPM, and one block more:
    // the beat's 24th pulse falls on the first sample after the beat.
    double ppq = 0.0;
    const double per_sample = 120.0 / 60.0 / RATE;
    for (uint32_t i = 0; i < 51; i++){
        const uint32_t n = clock.block(true, ppq, 120.0, RATE, 480, in, 256);
        e.process(480, in, n);
        ppq += per_sample * 480;
    }
    CHECK(e.master().clock().running());
    // 24 pulses have moved the count to the beat; the timer between them
    // may have added a subtick or two, never taken one away.
    const uint32_t count = e.master().clock().count();
    CHECK(count >= CLOCK_SUBTICKS_PER_QUARTER);
    CHECK(count < CLOCK_SUBTICKS_PER_QUARTER + CLOCK_SUBTICK);

    const uint32_t n = clock.block(false, ppq, 120.0, RATE, 480, in, 256);
    e.process(480, in, n);
    CHECK(!e.master().clock().running());
    const uint32_t stopped_at = e.master().clock().count();
    run_silent(e, 4800);
    CHECK_EQ(e.master().clock().count(), stopped_at);
}

// --- the editor's SysEx -------------------------------------------------------

static void test_sysex_from_the_editor_is_answered_to_the_editor(){
    Engine& e = fresh(engine_a);
    const std::vector<uint8_t> hello = framed(SYSEX_HELLO);
    e.receive_sysex(MIDI_CONTROL_PORT, hello.data(), (uint32_t)hello.size());
    const PluginMidiOut& out = e.midi();
    CHECK(out.editor_sysex_used > 5);
    CHECK_EQ(out.host_sysex_used, 0);
    if (out.editor_sysex_used > 5){
        CHECK_EQ(out.editor_sysex[0], 0xF0);
        CHECK_EQ(out.editor_sysex[3], SYSEX_IDENTITY);
        CHECK_EQ(out.editor_sysex[out.editor_sysex_used - 1], 0xF7);
    }
    // The same request on the host's wire is answered on the host's wire,
    // and the editor's buffer is untouched by it.
    const uint32_t editor_before = out.editor_sysex_used;
    e.receive_sysex(HOST_IN, hello.data(), (uint32_t)hello.size());
    CHECK(out.host_sysex_used > 5);
    CHECK_EQ(out.editor_sysex_used, editor_before);
}

static void test_an_edit_over_sysex_changes_what_is_running(){
    Engine& e = fresh(engine_a);
    load_thru(e);
    // SET_MIDI_PORT: index 0, flags (bit 0: an output), mask, channel, buses.
    // Re-point the output's channel override to 5 and hear the note on 5.
    const std::vector<uint8_t> set = framed(SYSEX_SET_MIDI_PORT, {0, 1, mmMIDI_USB_0, 5, 0x01, 0, 0});
    e.receive_sysex(MIDI_CONTROL_PORT, set.data(), (uint32_t)set.size());
    CHECK(e.midi().editor_sysex_used > 3 && e.midi().editor_sysex[3] == SYSEX_ACK);
    const InEvent in[] = { channel_event(0, HOST_IN, MIDI_NOTE_ON, 1, 60, 100) };
    e.process(48, in, 1);
    CHECK_EQ(e.midi().n_events, 1);
    if (e.midi().n_events == 1) CHECK_EQ(e.midi().events[0].status, 0x94);
}

// --- state --------------------------------------------------------------------

static void test_state_round_trips_the_patch_and_the_presets(){
    Engine& e = fresh(engine_a);
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{HOST_IN, 0, one_bus(0)};
    p.nodes[0] = node_config(ALGO_TRANSPOSE);
    p.nodes[0].in_buses[0] = one_bus(0);
    p.nodes[0].out_buses[0] = one_bus(1);
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{mmMIDI_USB_0, 0, one_bus(1)};
    GlobalSettings g = e.patches().globals();
    g.bpm = 97;
    CHECK_EQ(e.patches().apply(p, g, e.now()), APPLY_OK);
    CHECK_EQ(e.patches().save_slot(2), APPLY_OK);
    run_silent(e, 3 * 48000);             // the autosave writes slot 0

    std::vector<uint8_t> state(Engine::state_capacity());
    const size_t written = e.write_state(state.data(), state.size());
    CHECK(written > Engine::STATE_HEADER + EEPROM_BYTES);

    Engine& other = fresh(engine_b);
    CHECK(other.read_state(state.data(), written));
    CHECK_EQ(other.master().node_count(), 1);
    // An editor over the module hears that the patch under it changed.
    const PluginMidiOut& said = other.midi();
    CHECK(said.editor_sysex_used == 8 && said.editor_sysex[3] == SYSEX_EVENT
          && said.editor_sysex[5] == SYSEX_EVENT_PATCH_APPLIED);
    CHECK_EQ(other.patches().active().nodes[0].algorithm_id, ALGO_TRANSPOSE);
    CHECK_EQ(other.patches().globals().bpm, 97);
    CHECK(other.store().occupied(0));
    CHECK(other.store().occupied(2));
    CHECK(!other.patches().running_defaults());

    // Not this engine's bytes: refused, and the module runs regardless.
    uint8_t junk[64];
    memset(junk, 0x5A, sizeof junk);
    CHECK(!other.read_state(junk, sizeof junk));

    // An image that did not survive the trip is refused, and slot 0 of the
    // EEPROM that came with it - the same patch, autosaved - runs instead.
    std::vector<uint8_t> damaged = state;
    damaged[Engine::STATE_HEADER + 12] ^= 0x40;
    CHECK(!other.read_state(damaged.data(), written));
    CHECK_EQ(other.master().node_count(), 1);
    CHECK(!other.patches().running_defaults());
    run_silent(other, 2 * 48000);
    CHECK_EQ(other.leds().levels[LED_RED], 0);

    // Both refused - a state from another format version looks like this -
    // and the module boots as one flashed over a foreign EEPROM does: on its
    // defaults, with the red LED lit, still running.
    const size_t image = written - Engine::STATE_HEADER - EEPROM_BYTES;
    damaged[Engine::STATE_HEADER + image + 12] ^= 0x40;
    CHECK(!other.read_state(damaged.data(), written));
    CHECK(other.patches().running_defaults());
    CHECK_EQ(other.master().node_count(), 0);
    run_silent(other, 2 * 48000);
    CHECK(other.leds().levels[LED_RED] > 0);
}

// The window asks what the module has been doing and is answered on its own
// cable: the frame is the record since the last ask, so a note the track
// played is in it once, on the bus and on the way out, and gone from the next.
static void test_the_window_is_told_what_the_module_did(){
    Engine& e = fresh(engine_a);
    load_thru(e);
    const std::vector<uint8_t> ask = framed(SYSEX_MONITOR_REQUEST, {1, 0, 0, 0});   // note bus 0, no nodes
    e.receive_sysex(MIDI_CONTROL_PORT, ask.data(), (uint32_t)ask.size());
    e.midi().clear_editor_sysex();
    InEvent in[2] = {channel_event(0, HOST_IN, MIDI_NOTE_ON, 1, 60, 100),
                     channel_event(240, HOST_IN, MIDI_NOTE_OFF, 1, 60, 0)};
    e.process(480, in, 2);
    e.receive_sysex(MIDI_CONTROL_PORT, ask.data(), (uint32_t)ask.size());
    const PluginMidiOut& out = e.midi();
    CHECK(out.editor_sysex_used > 60);
    CHECK_EQ(out.editor_sysex[3], SYSEX_MONITOR);
    // Past the fixed part - at, clock, gates, jacks, LEDs, the CV buses, an
    // empty node list and the lost flag - is the event count.
    const size_t events_at = 5 + 5 + 1 + 2 + 5 + 5 + 5 + 8 + 4 + 3 * N_CV_BUS + 1 + 1;
    CHECK_EQ(out.editor_sysex[events_at], 4);           // on and off, on the bus and on the wire
    CHECK_EQ(out.editor_sysex[events_at + 1], 0);       // the first: on note bus 0
    CHECK_EQ(out.editor_sysex[events_at + 4], 60);
    CHECK_EQ(out.host_sysex_used, 0);                   // nothing of this reached the track
    e.midi().clear_editor_sysex();
    e.process(480, nullptr, 0);
    e.receive_sysex(MIDI_CONTROL_PORT, ask.data(), (uint32_t)ask.size());
    CHECK_EQ(e.midi().editor_sysex[events_at], 0);
}

static void test_nothing_is_allocated_while_running(){
    Engine& e = fresh(engine_a);
    load_thru(e);
    HostClock clock;
    InEvent in[256];
    const std::vector<uint8_t> hello = framed(SYSEX_HELLO);
    const size_t before = g_allocations;
    double ppq = 0.0;
    for (uint32_t i = 0; i < 100; i++){
        uint32_t n = clock.block(true, ppq, 128.0, RATE, 480, in, 200);
        in[n++] = channel_event(7, HOST_IN, MIDI_NOTE_ON, 1, 60, 100);
        in[n++] = channel_event(200, HOST_IN, MIDI_NOTE_OFF, 1, 60, 0);
        e.process(480, in, n);
        e.receive_sysex(MIDI_CONTROL_PORT, hello.data(), (uint32_t)hello.size());
        ppq += 128.0 / 60.0 / RATE * 480;
    }
    CHECK_EQ(g_allocations, before);
}

int main(){
    test_boots_on_the_default_patch_following_the_host();
    test_a_block_runs_the_passes_it_covers();
    test_a_message_is_heard_by_the_pass_that_covers_its_sample();
    test_a_message_after_the_last_pass_waits_for_the_next_block();
    test_a_message_from_the_window_takes_the_next_pass();
    test_program_change_is_two_bytes_and_realtime_one();
    test_host_clock_starts_and_pulses_on_the_grid();
    test_host_clock_rephases_on_a_loop_without_a_start();
    test_the_module_follows_the_host_transport();
    test_sysex_from_the_editor_is_answered_to_the_editor();
    test_an_edit_over_sysex_changes_what_is_running();
    test_state_round_trips_the_patch_and_the_presets();
    test_the_window_is_told_what_the_module_did();
    test_nothing_is_allocated_while_running();
    if (g_failures){ fprintf(stderr, "engine_test: %d failure(s)\n", g_failures); return 1; }
    printf("engine_test: ok\n");
    return 0;
}
