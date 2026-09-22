#include "engine.h"

#include "util/random.h"
#include "control/midi_dispatch.h"
#include "patch/patch_codec.h"
#include "patch/default_patch.h"
#include "hal/midi_types.h"

namespace mmmc_plugin {

// HostClock ------------------------------------------------------------------

HostClock::HostClock() : was_playing(false), expected_ppq(0.0), next_pulse(0) {}

void HostClock::reset(){
    was_playing = false;
    expected_ppq = 0.0;
    next_pulse = 0;
}

InEvent HostClock::realtime(uint32_t sample, uint8_t type){
    InEvent e;
    e.sample = sample; e.source = HOST_IN; e.type = type;
    e.channel = 0; e.d1 = 0; e.d2 = 0;
    return e;
}

// The pulse index of the first boundary strictly after `ppq`. A position
// exactly on a boundary is that boundary, which start() has already put the
// count on, so the next pulse is the one after it.
static int64_t pulse_after(double ppq){
    const double at = ppq * MASTER_PPQN;
    const int64_t floored = (int64_t)at;
    return (at < 0.0 && (double)floored != at) ? floored : floored + 1;
}

uint32_t HostClock::block(bool playing, double ppq, double bpm, double sample_rate,
                          uint32_t n_samples, InEvent* out, uint32_t capacity){
    uint32_t n = 0;
    if (!playing){
        if (was_playing && n < capacity) out[n++] = realtime(0, MIDI_STOP);
        was_playing = false;
        return n;
    }
    if (!was_playing){
        if (n < capacity) out[n++] = realtime(0, MIDI_START);
        next_pulse = pulse_after(ppq);
    } else {
        // A loop back or a relocate: the pulses re-phase onto the grid the
        // host is now on. A tick of tolerance, because two blocks' worth of
        // floating point do not meet exactly.
        const double drift = ppq - expected_ppq;
        if (drift > 1.0 / MASTER_PPQN || drift < -1.0 / MASTER_PPQN) next_pulse = pulse_after(ppq);
    }
    was_playing = true;

    if (bpm <= 0.0 || sample_rate <= 0.0 || n_samples == 0){
        expected_ppq = ppq;
        return n;
    }
    const double ppq_per_sample = bpm / 60.0 / sample_rate;
    const double end = ppq + ppq_per_sample * (double)n_samples;
    for (;;){
        const double at = (double)next_pulse / MASTER_PPQN;
        if (at >= end) break;
        double offset = (at - ppq) / ppq_per_sample;
        if (offset < 0.0) offset = 0.0;
        uint32_t sample = (uint32_t)(offset + 0.5);
        if (sample >= n_samples) sample = n_samples - 1;
        if (n < capacity) out[n++] = realtime(sample, MIDI_CLOCK);
        next_pulse++;
    }
    expected_ppq = end;
    return n;
}

// Engine ---------------------------------------------------------------------

Engine::Engine() :
    gpio(), midi_out(), eeprom(), led_driver(),
    mm(gpio, midi_out), status(led_driver), patch_store(eeprom),
    patch_manager(mm, patch_store, status), macros(),
    cc_map(patch_manager, mm, macros), control_sum(cc_map),
    mod_matrix(patch_manager, cc_map, control_sum),
    nrpn(patch_manager, cc_map),
    protocol(patch_manager, mm, patch_store, status, midi_out, cc_map, mod_matrix, macros, control_sum),
    queue(),
    now_us(0), subtick_interval(0), subtick_due(0),
    samples_per_pass(48.0), block_start(0.0), next_pass(0.0),
    last_beat(0), have_beat(false), booted(false) {}

void Engine::boot(uint32_t seed_a, uint32_t seed_b){
    // Before any node is constructed, as main.cpp does: a node that draws a
    // seed at construction must not play the same thing in every project.
    entropy::stir(seed_a);
    entropy::stir(seed_b);
    entropy::stir(seed_a ^ (seed_b << 7));
    run_boot();
    booted = true;
}

// As the module boots: slot 0 if it checks out, the default patch if not,
// and the red LED says which. A whole new patch either way, so every knob
// and macro position belonged to the one that went away
// (protocol/sysex_handler.cpp says the same at a load).
void Engine::run_boot(){
    patch_manager.boot(now_us);
    if (patch_manager.running_defaults()){
        // The module's default follows its own timer. A plugin's follows the
        // track it is on: nothing else on it ignores the play button.
        GlobalSettings g = patch_manager.globals();
        g.clock_source = MasterClock::CLOCK_MIDI;
        patch_manager.set_globals(g, now_us, false);
    }
    cc_map.reset();
    mod_matrix.reset();
    macros.reset();
}

void Engine::set_sample_rate(double samples_per_second){
    if (samples_per_second > 0.0) samples_per_pass = samples_per_second * PASS_US / 1000000.0;
    next_pass = block_start;
}

void Engine::enqueue(const InEvent& e){
    const MidiEvent m = {e.type, e.channel, e.d1, e.d2};
    queue.push(e.source, m);
}

void Engine::receive(uint8_t source, uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2){
    const MidiEvent m = {type, channel, d1, d2};
    queue.push(source, m);
}

void Engine::receive_sysex(uint8_t source, const uint8_t* data, uint32_t length){
    if (length > 0xFFFFu) return;
    protocol.deliver_sysex(source, data, (uint16_t)length, now_us);
}

void Engine::process(uint32_t n_samples, const InEvent* in, uint32_t n_in){
    midi_out.clear_events();
    midi_out.clear_host_sysex();
    const double end = block_start + (double)n_samples;
    uint32_t i = 0;
    while (next_pass < end){
        double at = next_pass - block_start;
        if (at < 0.0) at = 0.0;
        const uint32_t offset = (uint32_t)at;
        // Everything up to this pass's sample is what this pass hears, in
        // the order it arrived - a transport enqueues and the pass
        // dispatches (control/midi_dispatch.h).
        while (i < n_in && in[i].sample <= offset) enqueue(in[i++]);
        midi_out.at(offset);
        pass();
        next_pass += samples_per_pass;
    }
    // What arrived after the block's last pass waits for the first pass of
    // the next block: at most a pass late, never lost.
    while (i < n_in) enqueue(in[i++]);
    block_start = end;
}

// The interval timer and the sync pin, as the page runs them: one advance
// per subtick_interval_us() of simulated time, reprogrammed when the clock
// says the period moved (hal/teensy/teensy_clock.cpp does the same with an
// IntervalTimer).
void Engine::drive_clock(){
    MasterClock& clock = mm.clock();
    if (clock.take_interval_change()){
        subtick_interval = clock.subtick_interval_us();
        subtick_due = now_us + subtick_interval;
    }
    while (subtick_interval != 0 && (int32_t)(now_us - subtick_due) >= 0){
        clock.advance();
        subtick_due += subtick_interval;
    }
}

// main.cpp's loop, in main.cpp's order, minus the two things the host does
// for it: reading the transports (the block delivered them) and reprogramming
// a hardware timer (drive_clock is the timer).
void Engine::pass(){
    if (!booted) return;
    const uint32_t now = now_us;
    drive_clock();
    dispatch_midi(queue, protocol, nrpn, cc_map, mm, now);
    cc_map.apply(now);
    control_sum.begin();
    mod_matrix.apply(mm.buses(), now);
    macros.expand(patch_manager.active(), control_sum);
    control_sum.commit(now);
    mm.pass(now);

    const uint32_t beat = mm.clock().count() / CLOCK_SUBTICKS_PER_QUARTER;
    if (!have_beat || beat != last_beat){
        if (have_beat) status.beat(now);
        last_beat = beat;
        have_beat = true;
    }
    status.set_clock_running(mm.clock().running());
    status.service(now);
    protocol.service(now);
    nrpn.service(now);
    patch_manager.service(now);
    now_us = now + PASS_US;
}

// State ------------------------------------------------------------------

size_t Engine::state_capacity(){
    return STATE_HEADER + patch_codec::max_encoded_size() + EEPROM_BYTES;
}

static void put_u32(uint8_t* out, uint32_t v){
    out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);  out[3] = (uint8_t)v;
}
static uint32_t get_u32(const uint8_t* in){
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | in[3];
}

size_t Engine::write_state(uint8_t* out, size_t capacity){
    if (out == nullptr || capacity < state_capacity()) return 0;
    size_t image = 0;
    const CodecError e = patch_codec::encode(patch_manager.active(), patch_manager.globals(),
                                             out + STATE_HEADER, capacity - STATE_HEADER - EEPROM_BYTES, image);
    if (e != CODEC_OK) return 0;
    put_u32(out, STATE_MAGIC);
    put_u32(out + 4, (uint32_t)image);
    uint8_t* cells = out + STATE_HEADER + image;
    for (size_t i = 0; i < EEPROM_BYTES; i++) cells[i] = eeprom.cells[i];
    return STATE_HEADER + image + EEPROM_BYTES;
}

bool Engine::read_state(const uint8_t* in, size_t length){
    if (in == nullptr || length < STATE_HEADER + EEPROM_BYTES) return false;
    if (get_u32(in) != STATE_MAGIC) return false;
    const size_t image = get_u32(in + 4);
    if (STATE_HEADER + image + EEPROM_BYTES != length) return false;

    const uint8_t* cells = in + STATE_HEADER + image;
    for (size_t i = 0; i < EEPROM_BYTES; i++) eeprom.cells[i] = cells[i];

    // Into staging, never onto the stack: a Patch is over 11 KB. The
    // validator decides, and a refusal leaves the module on its defaults
    // rather than on half of somebody else's patch.
    const CodecError e = patch_codec::decode(in + STATE_HEADER, image,
                                             patch_manager.staging(), patch_manager.staging_globals());
    const bool ok = e == CODEC_OK && patch_manager.commit(now_us) == APPLY_OK;
    if (ok){
        cc_map.reset();
        mod_matrix.reset();
        macros.reset();
    } else {
        // Refused - another format version, or bytes that did not survive
        // the trip. Slot 0 of the EEPROM just copied in carries the same
        // patch, so booting from it is the module's own answer: a slot that
        // checks out runs, and one that does not lights the red LED over
        // the defaults, as a module flashed over a foreign EEPROM does.
        run_boot();
    }
    // Whatever is running now, an open editor was looking at something
    // else: the protocol tells it, and it re-reads the module.
    protocol.announce_patch_applied();
    return ok;
}

}
