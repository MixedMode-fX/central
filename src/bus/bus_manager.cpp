#include "bus/bus_manager.h"

BusManager::BusManager() :
    gate_front(0), gate_back(0), note_front(), note_back(), note_overflow(), cv_front(), cv_back()
{
    reset();
}

void BusManager::reset(){
    gate_front = gate_back = 0;
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        note_front[b].count = 0;
        note_back[b].count = 0;
        note_overflow[b] = 0;
    }
    for (uint8_t b = 0; b < N_CV_BUS; b++){
        cv_front[b] = 0;
        cv_back[b] = 0;
    }
}

// Gate --------------------------------------------------------------------

bool BusManager::gate_read(uint8_t bus) const {
    if (bus >= N_GATE_BUS) return false;
    return ((gate_front >> bus) & 1u) != 0;
}

void BusManager::gate_write(uint8_t bus, bool level){
    if (bus >= N_GATE_BUS) return;
    if (level) gate_back |= (1u << bus);
}

// Note --------------------------------------------------------------------

uint8_t BusManager::note_count(uint8_t bus) const {
    if (bus >= N_NOTE_BUS) return 0;
    return note_front[bus].count;
}

const MidiEvent& BusManager::note_read(uint8_t bus, uint8_t index) const {
    static const MidiEvent none = {0, 0, 0, 0};
    if (bus >= N_NOTE_BUS || index >= note_front[bus].count) return none;
    return note_front[bus].events[index];
}

bool BusManager::note_write(uint8_t bus, const MidiEvent& event){
    if (bus >= N_NOTE_BUS) return false;
    NoteQueue& q = note_back[bus];
    if (q.count >= NOTE_QUEUE_DEPTH){
        note_overflow[bus]++;
        return false;
    }
    q.events[q.count++] = event;
    return true;
}

uint8_t BusManager::note_room(uint8_t bus) const {
    if (bus >= N_NOTE_BUS) return 0;
    return (uint8_t)(NOTE_QUEUE_DEPTH - note_back[bus].count);
}

uint32_t BusManager::note_overflows(uint8_t bus) const {
    if (bus >= N_NOTE_BUS) return 0;
    return note_overflow[bus];
}

// CV ----------------------------------------------------------------------

int16_t BusManager::cv_read(uint8_t bus) const {
    if (bus >= N_CV_BUS) return 0;
    return cv_front[bus];
}

void BusManager::cv_write(uint8_t bus, int16_t value){
    if (bus >= N_CV_BUS) return;
    const int32_t sum = (int32_t)cv_back[bus] + (int32_t)value;
    if (sum > INT16_MAX)      cv_back[bus] = INT16_MAX;
    else if (sum < INT16_MIN) cv_back[bus] = INT16_MIN;
    else                      cv_back[bus] = (int16_t)sum;
}

// -------------------------------------------------------------------------

void BusManager::swap(){
    gate_front = gate_back;
    gate_back = 0;
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        const uint8_t n = note_back[b].count;
        for (uint8_t i = 0; i < n; i++) note_front[b].events[i] = note_back[b].events[i];
        note_front[b].count = n;
        note_back[b].count = 0;
    }
    for (uint8_t b = 0; b < N_CV_BUS; b++){
        cv_front[b] = cv_back[b];
        cv_back[b] = 0;
    }
}
