#include "midi/sounding_notes.h"
#include "hal/midi_types.h"

SoundingNotes::SoundingNotes() : notes(), none{0xFF, 0xFF, 0}, n(0), refusals(0) {}

void SoundingNotes::send_off(BusManager& bus, uint8_t out_bus, const SoundingNote& s) const {
    const MidiEvent off = {MIDI_NOTE_OFF, s.channel, s.note, 0};
    bus.note_write(out_bus, off);
}

bool SoundingNotes::emit(BusManager& bus, uint8_t out_bus,
                         uint8_t source, uint8_t note, uint8_t velocity, uint8_t channel){
    if (n >= CAPACITY){
        refusals++;
        return false;                       // never emit what cannot be released
    }
    notes[n].source = source;
    notes[n].note = note;
    notes[n].channel = channel;
    n++;
    // Velocity 0 would be a note-off: an emitted note-on is at least 1.
    const MidiEvent on = {MIDI_NOTE_ON, channel, note, (uint8_t)(velocity ? velocity : 1)};
    bus.note_write(out_bus, on);
    return true;
}

uint8_t SoundingNotes::release(BusManager& bus, uint8_t out_bus, uint8_t source){
    uint8_t released = 0;
    uint8_t i = 0;
    while (i < n){
        if (notes[i].source != source){ i++; continue; }
        send_off(bus, out_bus, notes[i]);
        for (uint8_t j = i; j + 1 < n; j++) notes[j] = notes[j + 1];
        n--;
        released++;
    }
    return released;
}

uint8_t SoundingNotes::release_all(BusManager& bus, uint8_t out_bus){
    const uint8_t released = n;
    for (uint8_t i = 0; i < n; i++) send_off(bus, out_bus, notes[i]);
    n = 0;
    return released;
}

const SoundingNote& SoundingNotes::at(uint8_t index) const {
    return index < n ? notes[index] : none;
}

bool SoundingNotes::holds(uint8_t source) const {
    for (uint8_t i = 0; i < n; i++) if (notes[i].source == source) return true;
    return false;
}
