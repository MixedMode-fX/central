#ifndef MMMC_BUS_BUS_MANAGER_H
#define MMMC_BUS_BUS_MANAGER_H

#include <stdint.h>
#include "config.h"
#include "bus/domain.h"

// All internal buses, double-buffered.
//
// Readers see the *front* buffer, which holds what was written during the
// previous pass. Writers accumulate into the *back* buffer. swap() publishes
// the back buffer and clears it. This makes evaluation order-independent,
// turns feedback into a well-defined one-pass delay instead of recursion,
// and makes every pass deterministic for the tests.
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
        uint32_t note_overflows(uint8_t bus) const;

        // CV --------------------------------------------------------------
        int16_t cv_read(uint8_t bus) const;
        void cv_write(uint8_t bus, int16_t value);

        // Publishes this pass's writes and clears the back buffer.
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
        NoteQueue note_front[N_NOTE_BUS];
        NoteQueue note_back[N_NOTE_BUS];
        uint32_t note_overflow[N_NOTE_BUS];
        int16_t cv_front[N_CV_BUS];
        int16_t cv_back[N_CV_BUS];
};

static_assert(N_GATE_BUS <= 32, "gate buses are held in a 32-bit word");

#endif
