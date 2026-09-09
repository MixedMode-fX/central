#include "algorithm/midi/arpeggiator.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[3] = {Domain::Note, Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor Arpeggiator::descriptor = {
    ALGO_ARPEGGIATOR, "Arpeggiator", 3, 2, 1, 4, IN, OUT, sizeof(Arpeggiator), false, construct_node<Arpeggiator> };

Arpeggiator::Arpeggiator(const NodeConfig& config) :
    held_in(config.in_bus[0]),
    advance_in(config.in_bus[1]),
    reset_in(config.in_bus[2]),
    out(config.out_bus[0]),
    mode(config.params[0]),
    octaves(config.params[1] == 0 ? 1 : (config.params[1] > MAX_OCTAVES ? MAX_OCTAVES : config.params[1])),
    gate_ms(config.params[2]),
    fixed_velocity(config.params[3]),
    cursor(0), playing(HeldNotes::NONE), started_us(0),
    descending(false), last_advance(false), last_reset(false),
    held(), sounding(), rng(entropy::seed())
{}

uint8_t Arpeggiator::steps() const {
    return (uint8_t)(held.count() * octaves);
}

void Arpeggiator::release(BusManager& bus){
    if (playing == HeldNotes::NONE) return;
    sounding.release(bus, out, playing);
    playing = HeldNotes::NONE;
}

// Turns a position in the figure into a note. The figure is the held notes in
// the configured order, repeated once per octave.
void Arpeggiator::note_for(uint8_t index, uint8_t& note, uint8_t& velocity, uint8_t& channel) const {
    const uint8_t count = held.count();
    note = HeldNotes::NONE;
    velocity = 0;
    channel = 1;
    if (count == 0) return;

    const uint8_t octave = (uint8_t)((index / count) % octaves);
    const uint8_t within = (uint8_t)(index % count);
    const HeldNote& h = (mode == ARP_AS_PLAYED) ? held.at(within) : held.sorted(within);
    if (h.note == HeldNotes::NONE) return;

    const int16_t shifted = (int16_t)h.note + (int16_t)octave * 12;
    if (shifted > 127) return;                       // skipped, never wrapped
    note = (uint8_t)shifted;
    velocity = fixed_velocity ? fixed_velocity : h.velocity;
    channel = h.channel;
}

void Arpeggiator::step(BusManager& bus, uint32_t now_us){
    const uint8_t total = steps();
    if (total == 0) return;

    uint8_t index = cursor;
    switch (mode){
        case ARP_DOWN:
            index = (uint8_t)(total - 1u - (cursor % total));
            cursor = (uint8_t)((cursor + 1u) % total);
            break;
        case ARP_UP_DOWN: {
            // Endpoints are not repeated, so a three-note chord plays
            // 1 2 3 2 rather than 1 2 3 3 2 1.
            if (cursor >= total){ cursor = 0; descending = false; }
            index = cursor;
            if (total == 1){ cursor = 0; break; }
            if (descending){
                if (cursor == 0){ descending = false; cursor = 1; }
                else cursor--;
            } else {
                if (cursor + 1u >= total){ descending = true; cursor = (uint8_t)(total - 2u); }
                else cursor++;
            }
            break;
        }
        case ARP_RANDOM:
            index = rng.below(total);
            cursor = index;
            break;
        default:                                     // up, and as-played
            index = (uint8_t)(cursor % total);
            cursor = (uint8_t)((cursor + 1u) % total);
            break;
    }

    uint8_t note = 0, velocity = 0, channel = 1;
    note_for(index, note, velocity, channel);
    if (note == HeldNotes::NONE) return;             // an octave off the top
    // The ledger is keyed on the note as sent: two steps can produce the same
    // pitch from different octaves, and each still gets its own release.
    if (sounding.emit(bus, out, note, note, velocity, channel)){
        playing = note;
        started_us = now_us;
    }
}

void Arpeggiator::process(BusManager& bus, uint32_t now_us){
    // The held chord.
    const uint8_t n = bus.note_count(held_in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(held_in, i);
        if (is_note_on(e)){
            HeldNote evicted = {0, 0, 0};
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
            if (did_evict) sounding.release(bus, out, evicted.note);
        } else if (is_note_off(e)){
            held.remove(e.data1);
        }
    }

    if (reset_in != NO_BUS){
        const bool level = bus.gate_read(reset_in);
        if (level && !last_reset){ cursor = 0; descending = false; }
        last_reset = level;
    }

    // Nothing held: release and stand down, so lifting the chord never leaves
    // a note sounding.
    if (held.count() == 0){
        release(bus);
        sounding.release_all(bus, out);
        cursor = 0;
        descending = false;
        last_advance = bus.gate_read(advance_in);
        return;
    }

    const bool level = bus.gate_read(advance_in);
    const bool rising = level && !last_advance;
    last_advance = level;

    // A fixed gate length releases on time rather than on the next step.
    if (gate_ms != 0 && playing != HeldNotes::NONE &&
        (uint32_t)(now_us - started_us) >= (uint32_t)gate_ms * 1000u){
        release(bus);
    }
    if (!rising) return;
    release(bus);
    step(bus, now_us);
}
