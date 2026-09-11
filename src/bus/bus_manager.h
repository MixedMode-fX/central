#ifndef MMMC_BUS_BUS_MANAGER_H
#define MMMC_BUS_BUS_MANAGER_H

#include <stdint.h>
#include "config.h"
#include "bus/domain.h"

// All internal buses, double-buffered.
//
// Readers see the *front* buffer; writers accumulate into the *back* buffer,
// and publishing a bus moves one to the other. Feedback is therefore a
// well-defined one-pass delay instead of recursion, and every pass is
// deterministic for the tests.
//
// **A bus is published the moment its last writer has run**, not at the end
// of the pass: the master runs the pool in an order where every writer of a
// bus precedes its readers (node/schedule.h), so a reader sees this pass's
// value rather than the previous one and a signal crosses the whole graph in
// the pass that produced it. Every bus is published exactly once a pass -
// one nothing writes any more empties rather than holding its last value -
// and a bus written again after its publish point, which is what a node
// releasing its notes during a handover does, is merged by the publish that
// ends the pass rather than replaced by it.
//
// Merge rules (applied in the write functions):
//   Gate: OR of all writers.
//   Note: append in arrival order; a full queue drops the newest event and
//         counts it in note_overflows().
//   CV:   sum with saturation to int16_t.
class BusManager {
    public:
        BusManager();

        // Gate ------------------------------------------------------------
        bool gate_read(uint8_t bus) const;
        void gate_write(uint8_t bus, bool level);

        // Note ------------------------------------------------------------
        uint8_t note_count(uint8_t bus) const;
        const MidiEvent& note_read(uint8_t bus, uint8_t index) const;
        bool note_write(uint8_t bus, const MidiEvent& event);
        // Events the bus can still take this pass before note_write() starts
        // dropping. What the input drain asks before it delivers, so a burst
        // larger than the bus waits in the queue rather than being counted
        // as overflow (#5).
        uint8_t note_room(uint8_t bus) const;
        uint32_t note_overflows(uint8_t bus) const;

        // CV --------------------------------------------------------------
        int16_t cv_read(uint8_t bus) const;
        void cv_write(uint8_t bus, int16_t value);

        // Publishes the selected buses - one bit per bus, per domain - and
        // clears their back buffers. The first publish of a bus in a pass
        // replaces what the readers see; a later one merges into it under
        // the domain's own fan-in rule.
        void publish(uint32_t gate_mask, uint16_t note_mask, uint16_t cv_mask);
        // Publishes everything not yet published and ends the pass.
        void swap();
        // Clears everything (patch load).
        void reset();

    private:
        struct NoteQueue {
            MidiEvent events[NOTE_QUEUE_DEPTH];
            uint8_t count;
        };

        uint32_t gate_front;
        uint32_t gate_back;
        // Which buses have already been published this pass, so that a
        // second publish merges into what the readers see instead of wiping
        // it.
        uint32_t gate_published;
        uint16_t note_published;
        uint16_t cv_published;
        NoteQueue note_front[N_NOTE_BUS];
        NoteQueue note_back[N_NOTE_BUS];
        uint32_t note_overflow[N_NOTE_BUS];
        int16_t cv_front[N_CV_BUS];
        int16_t cv_back[N_CV_BUS];
};

static_assert(N_GATE_BUS <= 32, "gate buses are held in a 32-bit word");
static_assert(N_NOTE_BUS <= 16, "a note bus mask is a 16-bit word");
static_assert(N_CV_BUS <= 16, "a CV bus mask is a 16-bit word");

#endif
