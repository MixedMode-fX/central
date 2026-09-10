#include "midi/held_notes.h"

HeldNotes::HeldNotes() : notes(), none{NONE, 0, 0}, n(0) {}

const HeldNote* HeldNotes::find(uint8_t note) const {
    for (uint8_t i = 0; i < n; i++) if (notes[i].note == note) return &notes[i];
    return nullptr;
}

bool HeldNotes::add(uint8_t note, uint8_t velocity, uint8_t channel,
                    HeldNote& evicted, bool& did_evict){
    did_evict = false;
    for (uint8_t i = 0; i < n; i++){
        if (notes[i].note != note) continue;
        notes[i].velocity = velocity;    // a repeat keeps its place in the order
        notes[i].channel = channel;
        return true;
    }
    if (n >= CAPACITY){
        evicted = notes[0];              // the oldest goes, and the caller
        did_evict = true;                // is told so it can release it
        for (uint8_t i = 0; i + 1 < n; i++) notes[i] = notes[i + 1];
        n--;
    }
    notes[n].note = note;
    notes[n].velocity = velocity;
    notes[n].channel = channel;
    n++;
    return true;
}

bool HeldNotes::remove(uint8_t note){
    for (uint8_t i = 0; i < n; i++){
        if (notes[i].note != note) continue;
        for (uint8_t j = i; j + 1 < n; j++) notes[j] = notes[j + 1];
        n--;
        return true;
    }
    return false;
}

void HeldNotes::clear(){ n = 0; }

const HeldNote& HeldNotes::at(uint8_t index) const {
    return index < n ? notes[index] : none;
}

const HeldNote& HeldNotes::sorted(uint8_t index) const {
    if (index >= n) return none;
    // The index-th lowest, by counting how many notes are below each one.
    // n is at most CAPACITY and a modifier asks for one note per edge, so a
    // scan is cheaper than keeping a second order up to date.
    for (uint8_t i = 0; i < n; i++){
        uint8_t below = 0;
        for (uint8_t j = 0; j < n; j++) if (notes[j].note < notes[i].note) below++;
        if (below == index) return notes[i];
    }
    return none;
}

uint8_t HeldNotes::lowest() const {
    if (n == 0) return NONE;
    uint8_t best = notes[0].note;
    for (uint8_t i = 1; i < n; i++) if (notes[i].note < best) best = notes[i].note;
    return best;
}

uint8_t HeldNotes::highest() const {
    if (n == 0) return NONE;
    uint8_t best = notes[0].note;
    for (uint8_t i = 1; i < n; i++) if (notes[i].note > best) best = notes[i].note;
    return best;
}

uint8_t HeldNotes::latest() const {
    return n == 0 ? NONE : notes[n - 1].note;
}

uint8_t HeldNotes::winner(NotePriorityRule rule) const {
    switch (rule){
        case NOTE_PRIORITY_HIGHEST: return highest();
        case NOTE_PRIORITY_LATEST:  return latest();
        default:                    return lowest();
    }
}
