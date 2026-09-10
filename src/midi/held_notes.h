#ifndef MMMC_MIDI_HELD_NOTES_H
#define MMMC_MIDI_HELD_NOTES_H

#include <stdint.h>
#include "config.h"

// Which notes are held, in what order, at what velocity (#10).
//
// Every modifier needs this and none of them should reinvent it: note
// priority wants the lowest or the highest, an arpeggiator wants them in
// arrival order or in pitch order, and a chord wants the root. One tested,
// allocation-free component serves all of them.
//
// Four rules, each of them a bug the repository's first attempt shipped:
//
//  1. Overflow is never a silent overwrite. add() reports the note it had to
//     evict, and the caller releases it.
//  2. "No note" has exactly one representation: NONE. Not 128 in one query
//     and 255 in another.
//  3. Arrival order is recorded, because "latest" priority and an as-played
//     arpeggio both need it and a flat array with a sentinel cannot express
//     it. Entry 0 is the oldest.
//  4. clear() forgets and nothing more. It emits no MIDI, so it can never
//     re-trigger a note the way a panic routine built on the note-off path
//     did.
struct HeldNote {
    uint8_t note;
    uint8_t velocity;
    uint8_t channel;
};

// Which held note a monophonic voice takes. Two algorithms ask this - the
// `NotePriority` modifier and the `MidiToCV` converter - and a third would be
// wrong to answer it a fourth way, so the question lives with the store that
// can answer it rather than in a switch in each of them. Every one of the
// three answers is wanted by somebody: `lowest` is a bass line, `highest` is a
// lead, `latest` is what a keyboard player expects.
//
// An algorithm's *parameter* is not this enum - the two that have one number
// their options differently, because a stored zero means the descriptor's
// default and their defaults differ - but both are defined in terms of these
// values so the mapping is a compile-time fact rather than a coincidence.
enum NotePriorityRule : uint8_t {
    NOTE_PRIORITY_LOWEST  = 0,
    NOTE_PRIORITY_HIGHEST = 1,
    NOTE_PRIORITY_LATEST  = 2,
    NOTE_PRIORITY_RULES   = 3,
};

class HeldNotes {
    public:
        static constexpr uint8_t NONE = 0xFF;
        static constexpr uint8_t CAPACITY = MAX_HELD_NOTES;

        HeldNotes();

        // Adds a note, or refreshes the velocity of one already held. When
        // the store is full the oldest note is evicted to make room: it is
        // written to `evicted` and `did_evict` is set, and the caller owes it
        // a note-off. Returns false only if the note could not be stored,
        // which cannot happen once eviction is allowed.
        bool add(uint8_t note, uint8_t velocity, uint8_t channel,
                 HeldNote& evicted, bool& did_evict);
        // Forgets one note. Returns false if it was not held.
        bool remove(uint8_t note);
        // Forgets everything. Emits nothing.
        void clear();

        uint8_t count() const { return n; }
        bool empty() const { return n == 0; }
        bool contains(uint8_t note) const { return find(note) != nullptr; }
        // The note as it is held - velocity and channel included - or nullptr
        // if it is not. What a caller that has chosen a note needs next, and
        // the reason no algorithm scans `at()` by hand to find one.
        const HeldNote* find(uint8_t note) const;

        // Arrival order: 0 is the oldest note still held.
        const HeldNote& at(uint8_t index) const;
        // Pitch order: 0 is the lowest note held. Computed rather than kept,
        // because arrival order is the one an insertion cannot preserve.
        const HeldNote& sorted(uint8_t index) const;

        uint8_t lowest() const;
        uint8_t highest() const;
        uint8_t latest() const;
        // Whichever of the three the rule asks for; NONE when nothing is held.
        uint8_t winner(NotePriorityRule rule) const;

    private:
        HeldNote notes[CAPACITY];
        HeldNote none;
        uint8_t n;
};

#endif
