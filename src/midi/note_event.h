#ifndef MMMC_MIDI_NOTE_EVENT_H
#define MMMC_MIDI_NOTE_EVENT_H

#include "bus/domain.h"
#include "hal/midi_types.h"

// A note-on with velocity 0 is a note-off. Every modifier has to agree about
// that, so it is written once here rather than in each of them.
inline bool is_note_on(const MidiEvent& e){
    return e.type == MIDI_NOTE_ON && e.data2 > 0;
}
inline bool is_note_off(const MidiEvent& e){
    return e.type == MIDI_NOTE_OFF || (e.type == MIDI_NOTE_ON && e.data2 == 0);
}
inline bool is_note(const MidiEvent& e){
    return e.type == MIDI_NOTE_ON || e.type == MIDI_NOTE_OFF;
}

#endif
