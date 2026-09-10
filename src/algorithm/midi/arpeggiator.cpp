#include "algorithm/midi/arpeggiator.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[4] = {Domain::Note, Domain::Gate, Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const char* const MODE_NAMES[5] = {"up", "down", "up-down", "random", "as played"};
static const ParamDescriptor PARAMS[5] = {
    {"mode",     0, 4,                       0, PARAM_ENUM,   MODE_NAMES},
    {"octaves",  1, Arpeggiator::MAX_OCTAVES, 1, PARAM_NUMBER, nullptr},
    {"gate",     0, 255,                     0, PARAM_MILLIS, nullptr},
    {"velocity", 0, 127,                     0, PARAM_NUMBER, nullptr},
    {"hold",     0, 1,                       0, PARAM_BOOL,   nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 5, PARAMS}};

static const char* const IN_NAMES[4] = {"chord in", "advance", "reset", "hold"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Arpeggiator::descriptor = {
    ALGO_ARPEGGIATOR, "Arpeggiator", 4, 2, 1, 5, IN, OUT, sizeof(Arpeggiator), false, construct_node<Arpeggiator>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Plays a held chord one note per advance edge, over a range of octaves. Hold latches it.",
    CATEGORY_MIDI };

// The cursor is a position in a figure whose length is held notes x octaves,
// and step() takes it modulo that length, so narrowing the octave range under
// a running arpeggio lands on a defined step at the next advance rather than
// immediately. The sounding note is released from the ledger either way.
bool Arpeggiator::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: if (value > ARP_AS_PLAYED) return false; mode = value; return true;
        case 1:
            if (value > MAX_OCTAVES) return false;
            octaves = value ? value : 1;
            return true;
        case 2: gate_ms = value; return true;
        case 3: fixed_velocity = value & 0x7F; return true;
        // Latching and unlatching happen in process(), on the edge of the
        // parameter and the inlet together, so writing this is the same
        // operation as a footswitch going down.
        case 4: hold = value ? 1u : 0u; return true;
        default: return false;
    }
}

uint8_t Arpeggiator::get_param(uint16_t index) const {
    switch (index){
        case 0: return mode;
        case 1: return octaves;
        case 2: return (uint8_t)(gate_ms > 255 ? 255 : gate_ms);
        case 3: return fixed_velocity;
        case 4: return hold;
        default: return 0;
    }
}

Arpeggiator::Arpeggiator(const NodeConfig& config) :
    held_in(config.in_bus[0]),
    advance_in(config.in_bus[1]),
    reset_in(config.in_bus[2]),
    hold_in(config.in_bus[3]),
    out(config.out_bus[0]),
    mode(config.params[0]),
    octaves(config.params[1] == 0 ? 1 : (config.params[1] > MAX_OCTAVES ? MAX_OCTAVES : config.params[1])),
    gate_ms(config.params[2]),
    fixed_velocity(config.params[3]),
    hold(config.params[4] ? 1u : 0u),
    cursor(0), playing(HeldNotes::NONE), started_us(0),
    descending(false), last_advance(false), last_reset(false), holding(false),
    down{0, 0, 0, 0}, down_count(0),
    held(), sounding(), rng(entropy::seed())
{}

bool Arpeggiator::key_down(uint8_t note) const {
    const uint8_t n = (uint8_t)(note & 0x7F);
    return (down[n >> 5] & (uint32_t)(1u << (n & 31u))) != 0;
}

void Arpeggiator::set_key(uint8_t note, bool is_down){
    const uint8_t n = (uint8_t)(note & 0x7F);
    const uint32_t bit = (uint32_t)(1u << (n & 31u));
    const bool was = (down[n >> 5] & bit) != 0;
    if (was == is_down) return;               // a repeated note-on is one key
    if (is_down){ down[n >> 5] |= bit; down_count++; }
    else { down[n >> 5] &= ~bit; down_count--; }
}

// Hold coming off: the figure keeps the keys that are still down and loses
// the rest. The sounding note is released by the ordinary path - either the
// next advance, or the stand-down below when nothing is left held - so this
// only edits the figure.
void Arpeggiator::drop_latch(BusManager& bus){
    for (uint8_t i = held.count(); i > 0; i--){
        const uint8_t note = held.at((uint8_t)(i - 1u)).note;
        if (note == HeldNotes::NONE || key_down(note)) continue;
        held.remove(note);
        sounding.release(bus, out, note);
        if (playing == note) playing = HeldNotes::NONE;
    }
}

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
    // Hold is the parameter or the inlet: either one on its own latches, so
    // a footswitch and an editor do not have to agree about who owns it.
    const bool hold_now = hold != 0 || (hold_in != NO_BUS && bus.gate_read(hold_in));
    // On the falling edge, before this pass's note events, so a note-off
    // arriving in the same pass as the release still takes its key away.
    if (holding && !hold_now) drop_latch(bus);
    holding = hold_now;

    // The held chord.
    const uint8_t n = bus.note_count(held_in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(held_in, i);
        if (is_note_on(e)){
            // Latched, and this is the first key of a new chord: the figure
            // is replaced, not added to.
            if (hold_now && down_count == 0 && held.count() != 0){
                held.clear();
                sounding.release_all(bus, out);
                playing = HeldNotes::NONE;
                cursor = 0;
                descending = false;
            }
            set_key(e.data1, true);
            HeldNote evicted = {0, 0, 0};
            bool did_evict = false;
            held.add(e.data1, e.data2, e.channel, evicted, did_evict);
            if (did_evict) sounding.release(bus, out, evicted.note);
        } else if (is_note_off(e)){
            set_key(e.data1, false);
            // Under hold the key is remembered as up and the note stays in
            // the figure; that is the whole feature.
            if (!hold_now) held.remove(e.data1);
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

void Arpeggiator::silence(BusManager& bus){
    sounding.release_all(bus, out);
    playing = HeldNotes::NONE;
}
