#ifndef MMMC_MIDI_SOUNDING_NOTES_H
#define MMMC_MIDI_SOUNDING_NOTES_H

#include <stdint.h>
#include "config.h"
#include "bus/bus_manager.h"

// What a modifier has emitted and still owes a note-off (#10).
//
// **Every modifier owns the note-offs for every note-on it emitted, and must
// release them using the transformation it originally applied - not the
// current parameter value.** Transpose is the simplest algorithm where that
// bites: change the offset while notes are held and a note-off computed from
// the new offset never matches the note-on already sent, so the note hangs on
// the downstream synth for ever. The same is true of a quantiser whose root
// moves, an arpeggiator whose octave range changes, and a sequencer whose
// scale changes under a sounding note.
//
// So a modifier does not recompute a release: it looks up what it actually
// sent. Each record maps the *source* note (what arrived) to the *emitted*
// note (what was sent) and the channel it went out on. One source may have
// several records - Chord emits an interval set from one note - so this is a
// list, not a map.
//
// Capacity is a hard limit on what may be emitted: a modifier that cannot
// record an emission does not make it. Never emitting what cannot be released
// is the only policy that keeps "no hanging notes" true under overflow.
struct SoundingNote {
    uint8_t source;    // the note that caused it
    uint8_t note;      // what was actually sent
    uint8_t channel;   // where it was sent
};

class SoundingNotes {
    public:
        static constexpr uint8_t CAPACITY = MAX_SOUNDING_NOTES;

        SoundingNotes();

        // Emits a note-on and records it. Returns false, having sent nothing,
        // when there is no room to record the release.
        bool emit(BusManager& bus, uint8_t out_bus,
                  uint8_t source, uint8_t note, uint8_t velocity, uint8_t channel);
        // Releases everything emitted for `source`. Returns how many.
        uint8_t release(BusManager& bus, uint8_t out_bus, uint8_t source);
        // Releases everything. Emits note-offs and nothing else.
        uint8_t release_all(BusManager& bus, uint8_t out_bus);

        uint8_t count() const { return n; }
        const SoundingNote& at(uint8_t index) const;
        bool holds(uint8_t source) const;
        // Emissions refused for want of a record.
        uint32_t refused() const { return refusals; }

    private:
        void send_off(BusManager& bus, uint8_t out_bus, const SoundingNote& s) const;

        SoundingNote notes[CAPACITY];
        SoundingNote none;
        uint8_t n;
        uint32_t refusals;
};

#endif
