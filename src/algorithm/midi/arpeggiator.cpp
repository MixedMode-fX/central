#include "algorithm/midi/arpeggiator.h"
#include "node/registry.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Note, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

const AlgorithmDescriptor Arpeggiator::descriptor = {
    ALGO_ARPEGGIATOR, "Arpeggiator", 2, 2, 1, 0, IN, OUT, sizeof(Arpeggiator), false, construct_node<Arpeggiator> };

Arpeggiator::Arpeggiator(const NodeConfig& config) :
    held_in(config.in_bus[0]),
    advance_in(config.in_bus[1]),
    out(config.out_bus[0]),
    notes(), velocities(), count(0), index(0), sounding(NONE), channel(1), last_gate(false)
{}

void Arpeggiator::add(uint8_t note, uint8_t velocity, uint8_t ch){
    for (uint8_t i = 0; i < count; i++) if (notes[i] == note) return;
    if (count >= MAX_HELD) return;
    uint8_t pos = count;
    while (pos > 0 && notes[pos - 1] > note){
        notes[pos] = notes[pos - 1];
        velocities[pos] = velocities[pos - 1];
        pos--;
    }
    notes[pos] = note;
    velocities[pos] = velocity;
    count++;
    channel = ch;
}

void Arpeggiator::remove(uint8_t note){
    for (uint8_t i = 0; i < count; i++){
        if (notes[i] != note) continue;
        for (uint8_t j = i; j + 1 < count; j++){
            notes[j] = notes[j + 1];
            velocities[j] = velocities[j + 1];
        }
        count--;
        return;
    }
}

void Arpeggiator::release(BusManager& bus){
    if (sounding == NONE) return;
    const MidiEvent off = {MIDI_NOTE_OFF, channel, sounding, 0};
    bus.note_write(out, off);
    sounding = NONE;
}

void Arpeggiator::process(BusManager& bus, uint32_t){
    // Track the held chord.
    const uint8_t n = bus.note_count(held_in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent& e = bus.note_read(held_in, i);
        if (e.type == MIDI_NOTE_ON && e.data2 > 0) add(e.data1, e.data2, e.channel);
        else if (e.type == MIDI_NOTE_OFF || e.type == MIDI_NOTE_ON) remove(e.data1);
    }

    // Advance on a rising edge.
    const bool gate = bus.gate_read(advance_in);
    const bool rising = gate && !last_gate;
    last_gate = gate;

    if (count == 0){
        release(bus);
        index = 0;
        return;
    }
    if (!rising) return;

    release(bus);
    if (index >= count) index = 0;
    const MidiEvent on = {MIDI_NOTE_ON, channel, notes[index], velocities[index]};
    bus.note_write(out, on);
    sounding = notes[index];
    index++;
}
