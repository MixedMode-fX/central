// The cascade is a port of chris_shifter() in Critter & Guitari's pparp
// (github.com/critterandguitari/pparp), which carries this notice:
//
// Copyright (c) 2016, Owen Osborn, Critter & Guitari
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "algorithm/midi/pocket_arp.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[3] = {Domain::Note, Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const char* const PATTERN_NAMES[1] = {"cascade"};
static const ParamDescriptor PARAMS[PocketArp::N_PARAMS] = {
    {"pattern",  0, 0,                        0, PARAM_ENUM,   PATTERN_NAMES},
    {"gate",     0, 255,                      0, PARAM_MILLIS, nullptr},
    {"velocity", 0, 127,                      0, PARAM_NUMBER, nullptr},
    {"jitter",   0, PocketArp::MAX_JITTER_MS, 0, PARAM_MILLIS, nullptr},
    {"channel",  0, 16,                       0, PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, PocketArp::N_PARAMS, PARAMS}};

static const char* const IN_NAMES[3] = {"chord in", "advance", "reset"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor PocketArp::descriptor = {
    ALGO_POCKET_ARP, "PocketArp", 3, 2, 1, PocketArp::N_PARAMS, IN, OUT,
    sizeof(PocketArp), false, construct_node<PocketArp>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Pocket Piano patterns. Cascade strikes every held note per advance, each in its own octave.",
    CATEGORY_MIDI };

bool PocketArp::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: if (value > POCKET_CASCADE) return false; pattern = value; return true;
        case 1: gate_ms = value; return true;
        case 2: fixed_velocity = value & 0x7F; return true;
        // Heard from the next strike: one already waiting keeps its time.
        case 3: if (value > MAX_JITTER_MS) return false; jitter_ms = value; return true;
        // A strike records the channel it went out on, so nothing is stranded.
        case 4: if (value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t PocketArp::get_param(uint16_t index) const {
    switch (index){
        case 0: return pattern;
        case 1: return (uint8_t)(gate_ms > 255 ? 255 : gate_ms);
        case 2: return fixed_velocity;
        case 3: return jitter_ms;
        case 4: return channel;
        default: return 0;
    }
}

PocketArp::PocketArp(const NodeConfig& config) :
    held_in(config.in_buses[0]),
    advance_in(config.in_buses[1]),
    reset_in(config.in_buses[2]),
    out(config.out_buses[0]),
    pattern(config.params[P_PATTERN] > POCKET_CASCADE ? (uint8_t)POCKET_CASCADE : config.params[P_PATTERN]),
    gate_ms(config.params[P_GATE]),
    fixed_velocity(config.params[P_VELOCITY] & 0x7F),
    jitter_ms(config.params[P_JITTER] > MAX_JITTER_MS ? MAX_JITTER_MS : config.params[P_JITTER]),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    rotation(0), shape(0), shifts{0, 0, 0, 0},
    last_advance(false), last_reset(false),
    voices(), strikes(), sounding(), rng(entropy::seed())
{
    for (Voice& v : voices) v = {NONE, 0, 1};
    for (Strike& s : strikes) s = {NONE, 0, 0, 1, false, 0};
}

// The first free slot, in the order keys go down; a key already in one, or
// one past the last slot, is ignored.
void PocketArp::take(uint8_t note, uint8_t velocity, uint8_t from){
    for (const Voice& v : voices) if (v.note == note) return;
    for (Voice& v : voices){
        if (v.note != NONE) continue;
        v = {note, velocity, from};
        return;
    }
}

void PocketArp::free_voice(uint8_t note){
    for (Voice& v : voices) if (v.note == note) v.note = NONE;
}

// chris_shifter(), less its output: which octave each voice is thrown to.
// The rotation runs round four and the shape round three, both moved before
// they are read, so a fresh node's first strike is rotation 1, shape 1.
void PocketArp::advance_cascade(){
    rotation = (uint8_t)((rotation + 1u) & 3u);
    shape = (uint8_t)(shape >= 2 ? 0 : shape + 1u);
    for (uint8_t i = 0; i < POCKET_VOICES; i++){
        switch (shape){
            case 0:  shifts[i] = (uint8_t)((rotation + i) & 3u); break;           // each its own
            case 1:  shifts[i] = (uint8_t)((rotation + (i & 1u) * 2u) & 3u); break; // in pairs
            default: shifts[i] = rotation; break;                                 // together
        }
    }
}

void PocketArp::release_strikes(BusManager& bus){
    sounding.release_all(bus, out);
    for (Strike& s : strikes){ s.source = NONE; s.sounding = false; }
}

// Every held voice at its octave. Two voices can land on one pitch - a key
// and its octave, thrown a step apart - and the second is not struck, so
// nothing downstream sees one note-on twice.
void PocketArp::strike(uint32_t now_us){
    advance_cascade();
    for (uint8_t i = 0; i < POCKET_VOICES; i++){
        const Voice& v = voices[i];
        if (v.note == NONE) continue;
        const int16_t shifted = (int16_t)v.note + 24 - 12 * (int16_t)shifts[i];
        if (shifted < 0 || shifted > 127) continue;               // skipped, never wrapped
        const uint8_t note = (uint8_t)shifted;
        const uint8_t sent_on = out_channel(channel, v.channel);
        bool taken = false;
        for (uint8_t j = 0; j < i; j++){
            const Strike& s = strikes[j];
            if (s.source != NONE && s.note == note && s.channel == sent_on) taken = true;
        }
        if (taken) continue;
        const uint8_t late_ms = jitter_ms ? rng.below((uint8_t)(jitter_ms + 1u)) : 0;
        strikes[i] = {v.note, note, fixed_velocity ? fixed_velocity : v.velocity,
                      sent_on, false, now_us + (uint32_t)late_ms * 1000u};
    }
}

void PocketArp::process(BusManager& bus, uint32_t now_us){
    const uint8_t n = bus.note_count(held_in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(held_in, i);
        if (is_note_on(e)) take(e.data1, e.data2, e.channel);
        else if (is_note_off(e)) free_voice(e.data1);
    }

    if (reset_in.any()){
        const bool level = bus.gate_read(reset_in);
        if (level && !last_reset){ rotation = 0; shape = 0; }
        last_reset = level;
    }

    const bool level = bus.gate_read(advance_in);
    const bool rising = level && !last_advance;
    last_advance = level;

    bool any_held = false;
    for (const Voice& v : voices) if (v.note != NONE) any_held = true;
    // Nothing held: stand down, so lifting the chord never leaves a note on.
    if (!any_held){
        release_strikes(bus);
        return;
    }

    if (rising){
        release_strikes(bus);
        strike(now_us);
    }

    for (Strike& s : strikes){
        if (s.source == NONE) continue;
        if (!s.sounding){
            if ((int32_t)(now_us - s.at_us) < 0) continue;        // its jitter
            // A note the ledger has no room for is never sent.
            if (!sounding.emit(bus, out, s.source, s.note, s.velocity, s.channel)){
                s.source = NONE;
                continue;
            }
            s.sounding = true;
            s.at_us = now_us;
        } else if (gate_ms != 0 && (uint32_t)(now_us - s.at_us) >= (uint32_t)gate_ms * 1000u){
            sounding.release(bus, out, s.source);
            s.source = NONE;
            s.sounding = false;
        }
    }
}

void PocketArp::silence(BusManager& bus){
    release_strikes(bus);
}
