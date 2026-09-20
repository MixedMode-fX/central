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

// **Every node that emits MIDI says which channel it emits on, and a node
// that is handed a stream says it by leaving the stream alone.** A modifier
// has a channel to inherit and a generator has not, so the two say it
// differently: a generator's `channel` is 1..16 and one of them is always
// chosen, while a modifier's is 0..16 and zero - the default, so an untouched
// preset is the behaviour everything had before there was a control - means
// the message leaves on the channel it arrived on. That is the only sensible
// default for a transposer or an arpeggiator: a keyboard split across two
// channels stays split, and a patch that wants it collected says so.
//
// Overriding it is not the same as putting a `Channel` node after the
// modifier, and that is why it is here rather than left to one: `Channel`
// re-addresses a whole bus, so a patch that merges two parts onto one bus
// cannot move just one of them without splitting the bus first. The override
// moves what this node emits and nothing else.
//
// The note-off is the trap, and it is not this function's to solve: a note
// that went out on channel 1 and is released on channel 5 never stops. Every
// node using this records the channel it emitted with (midi/sounding_notes.h)
// and releases from that record, so the override may move under a held note.
constexpr uint8_t CHANNEL_FROM_SOURCE = 0;

inline uint8_t out_channel(uint8_t override_channel, uint8_t source_channel){
    return override_channel ? override_channel : source_channel;
}

// The same decision for a message this node is passing through rather than
// emitting - a CC, a bend, aftertouch. It carries no note, so there is
// nothing to release and nothing to record.
inline MidiEvent readdressed(const MidiEvent& e, uint8_t override_channel){
    MidiEvent moved = e;
    moved.channel = out_channel(override_channel, e.channel);
    return moved;
}

#endif
