#include "bus/bus_manager.h"

BusManager::BusManager() :
    gate_front(0), gate_back(0), gate_published(0), note_published(0), cv_published(0),
    note_front(), note_back(), note_overflow(), cv_front(), cv_back()
{
    reset();
}

void BusManager::reset(){
    gate_front = gate_back = 0;
    gate_published = 0;
    note_published = 0;
    cv_published = 0;
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

bool BusManager::gate_read(BusSet set) const {
    return (gate_front & set.bits) != 0;
}

void BusManager::gate_write(BusSet set, bool level){
    if (level) gate_back |= (uint32_t)(set.bits & all_buses(Domain::Gate).bits);
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

uint8_t BusManager::note_count(BusSet set) const {
    uint16_t total = 0;
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        if (set.has(b)) total = (uint16_t)(total + note_front[b].count);
    }
    return total > 0xFFu ? (uint8_t)0xFFu : (uint8_t)total;
}

const MidiEvent& BusManager::note_read(BusSet set, uint8_t index) const {
    static const MidiEvent none = {0, 0, 0, 0};
    uint8_t at = index;
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        if (!set.has(b)) continue;
        if (at < note_front[b].count) return note_front[b].events[at];
        at = (uint8_t)(at - note_front[b].count);
    }
    return none;
}

bool BusManager::note_write(BusSet set, const MidiEvent& event){
    bool all = true;
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        if (set.has(b) && !note_write(b, event)) all = false;
    }
    return all;
}

uint8_t BusManager::note_room(uint8_t bus) const {
    if (bus >= N_NOTE_BUS) return 0;
    return (uint8_t)(NOTE_QUEUE_DEPTH - note_back[bus].count);
}

uint8_t BusManager::note_room(BusSet set) const {
    uint8_t room = NOTE_QUEUE_DEPTH;
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        if (!set.has(b)) continue;
        const uint8_t here = note_room(b);
        if (here < room) room = here;
    }
    return room;
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

int16_t BusManager::cv_read(BusSet set) const {
    int32_t sum = 0;
    for (uint8_t b = 0; b < N_CV_BUS; b++) if (set.has(b)) sum += (int32_t)cv_front[b];
    if (sum > INT16_MAX) return INT16_MAX;
    if (sum < INT16_MIN) return INT16_MIN;
    return (int16_t)sum;
}

void BusManager::cv_write(BusSet set, int16_t value){
    for (uint8_t b = 0; b < N_CV_BUS; b++) if (set.has(b)) cv_write(b, value);
}

// -------------------------------------------------------------------------

void BusManager::publish(uint32_t gate_mask, uint16_t note_mask, uint16_t cv_mask){
    // Gates. A bus published for the first time this pass starts from
    // nothing, so one whose writer has stopped writing falls low; one
    // published again ORs the late writes into what is already there.
    const uint32_t first_gate = gate_mask & ~gate_published;
    gate_front &= ~first_gate;
    gate_front |= (gate_back & gate_mask);
    gate_back &= ~gate_mask;
    gate_published |= gate_mask;

    for (uint8_t b = 0; note_mask != 0 && b < N_NOTE_BUS; b++){
        if ((note_mask & (1u << b)) == 0) continue;
        if ((note_published & (1u << b)) == 0) note_front[b].count = 0;
        const uint8_t waiting = note_back[b].count;
        for (uint8_t i = 0; i < waiting; i++){
            if (note_front[b].count >= NOTE_QUEUE_DEPTH){ note_overflow[b]++; continue; }
            note_front[b].events[note_front[b].count++] = note_back[b].events[i];
        }
        note_back[b].count = 0;
        note_published = (uint16_t)(note_published | (1u << b));
    }

    for (uint8_t b = 0; cv_mask != 0 && b < N_CV_BUS; b++){
        if ((cv_mask & (1u << b)) == 0) continue;
        if ((cv_published & (1u << b)) == 0) cv_front[b] = 0;
        const int32_t sum = (int32_t)cv_front[b] + (int32_t)cv_back[b];
        if (sum > INT16_MAX)      cv_front[b] = INT16_MAX;
        else if (sum < INT16_MIN) cv_front[b] = INT16_MIN;
        else                      cv_front[b] = (int16_t)sum;
        cv_back[b] = 0;
        cv_published = (uint16_t)(cv_published | (1u << b));
    }
}

void BusManager::swap(){
    publish(0xFFFFFFFFu, 0xFFFFu, 0xFFFFu);
    gate_published = 0;
    note_published = 0;
    cv_published = 0;
}
