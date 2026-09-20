#include "algorithm/midi/retrigger.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Note, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const char* const RELEASE_NAMES[Retrigger::RT_RELEASES] = {"length", "tie"};

static const ParamDescriptor PARAMS[Retrigger::N_PARAMS] = {
    {"length",   DIV_8_BARS,    DIVISIONS, DIV_16TH,      PARAM_ENUM,   DIVISION_NAMES},
    {"feel",     FEEL_STRAIGHT, FEELS,     FEEL_STRAIGHT, PARAM_ENUM,   FEEL_NAMES},
    {"release",  Retrigger::RT_LENGTH, Retrigger::RT_RELEASES, Retrigger::RT_LENGTH,
                                                          PARAM_ENUM,   RELEASE_NAMES},
    {"velocity", 0,             127,       0,             PARAM_NUMBER, nullptr},
    {"channel",  0,             16,        0,             PARAM_CHANNEL_OUT, nullptr},
};
// The first four describe one thing - what a strike is - so the editor is
// told that rather than left to sort them by what their names sound like,
// which files a note value under "timing", a release under "other" and a
// velocity under "level": three headings over a node with four controls
// (src/node/param.h). The channel is not part of a strike and is left out of
// the label, so it lands under the editor's own MIDI heading.
static const char* const STRIKE = "the strike";
static const ParamGroup GROUPS[2] = {
    {0, 1, 4, PARAMS, STRIKE},
    {Retrigger::P_CHANNEL, 1, 1, &PARAMS[Retrigger::P_CHANNEL]},
};

static const char* const IN_NAMES[2] = {"chord in", "trigger"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Retrigger::descriptor = {
    ALGO_RETRIGGER, "Retrigger", 2, 2, 1, Retrigger::N_PARAMS, IN, OUT, sizeof(Retrigger), true,
    construct_node<Retrigger>, GROUPS, 2, IN_NAMES, OUT_NAMES,
    "Re-strikes a held chord on every trigger. The rhythm is the trigger's, the length a note value.",
    CATEGORY_MIDI };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Retrigger::Retrigger(const NodeConfig& config) :
    in(config.in_buses[0]),
    out(config.out_buses[0]),
    length(clamp_enum(config.params[P_LENGTH], DIVISIONS, DIV_16TH)),
    how(clamp_enum(config.params[P_FEEL], FEELS, FEEL_STRAIGHT)),
    release(clamp_enum(config.params[P_RELEASE], RT_RELEASES, RT_LENGTH)),
    fixed_velocity((uint8_t)(config.params[P_VELOCITY] & 0x7F)),
    channel(config.params[P_CHANNEL] > 16 ? CHANNEL_FROM_SOURCE : config.params[P_CHANNEL]),
    subtick(0), off_at(0), struck(0),
    trigger_in(config.in_buses[1]),
    held(), sounding()
{}

void Retrigger::release_all(BusManager& bus){
    sounding.release_all(bus, out);
}

// The whole chord, in pitch order, with a ledger entry each: a strike is
// released by looking up what was sent (#10), so the velocity parameter can
// move under a sounding chord and the note-offs still match.
void Retrigger::strike(BusManager& bus){
    release_all(bus);
    const uint8_t n = held.count();
    if (n == 0) return;
    for (uint8_t i = 0; i < n; i++){
        const HeldNote& h = held.sorted(i);
        if (h.note == HeldNotes::NONE) continue;
        const uint8_t velocity = fixed_velocity ? fixed_velocity : h.velocity;
        sounding.emit(bus, out, h.note, h.note, velocity, out_channel(channel, h.channel));
    }
    off_at = subtick + length_subticks();
    struck++;
}

void Retrigger::tick(BusManager&, uint32_t count){ subtick = count; }

void Retrigger::process(BusManager& bus, uint32_t){
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            // Immediate, unlike the note-on: a key that has been lifted is
            // not part of the next strike and should not be part of this one.
            held.remove(e.data1);
            sounding.release(bus, out, e.data1);
            continue;
        }
        if (is_note_on(e)){
            HeldNote evicted = {0, 0, 0};
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
            // The chord is wider than the store, so the oldest note leaves it
            // - and a note that has left the chord owes its note-off.
            if (did_evict) sounding.release(bus, out, evicted.note);
            continue;
        }
        // A CC, a bend, aftertouch: not a note, so not this node's to hold.
        bus.note_write(out, readdressed(e, channel));
    }

    // The length is counted in subticks from the strike. An edit to `length`
    // does not move a release already scheduled, exactly as a new division
    // does not move a Metronome's next pulse: the articulation a musician is
    // hearing completes, and the next strike is the new one.
    if (release == RT_LENGTH && sounding.count() != 0 &&
        (uint32_t)(subtick - off_at) < 0x80000000u){
        release_all(bus);
    }

    // Sampled every pass, so an edge is never missed and never seen twice.
    if (trigger_in.rising(bus)) strike(bus);
}

void Retrigger::silence(BusManager& bus){
    release_all(bus);
}

bool Retrigger::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_LENGTH: if (value == 0 || value > DIVISIONS) return false; length = value; return true;
        case P_FEEL: if (value == 0 || value > FEELS) return false; how = value; return true;
        // Turning `tie` on leaves the strike sounding and turning it off
        // hands it back to a deadline that has already passed, so the next
        // pass releases it: either way the change is heard on the next
        // strike rather than by cutting the one in progress.
        case P_RELEASE: if (value == 0 || value > RT_RELEASES) return false; release = value; return true;
        case P_VELOCITY: if (value > 127) return false; fixed_velocity = value; return true;
        // The strike in the air was recorded with the channel it went out
        // on, so this too is heard on the next strike and strands nothing.
        case P_CHANNEL: if (value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t Retrigger::get_param(uint16_t index) const {
    switch (index){
        case P_LENGTH:   return length;
        case P_FEEL:     return how;
        case P_RELEASE:  return release;
        case P_VELOCITY: return fixed_velocity;
        case P_CHANNEL:  return channel;
        default: return 0;
    }
}
