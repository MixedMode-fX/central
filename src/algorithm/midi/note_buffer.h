#ifndef MMMC_ALGORITHM_MIDI_NOTE_BUFFER_H
#define MMMC_ALGORITHM_MIDI_NOTE_BUFFER_H

#include "node/node.h"
#include "algorithm/sequencer/step_engine.h"
#include "midi/sounding_notes.h"

// Every note that comes in stays on until it is cleared.
//
// A note-on is sent on at once and then held: its note-off is swallowed, so a
// chord played and let go keeps sounding, and the next note played joins it
// rather than replacing it. A rising edge on `reset` releases everything held,
// which is the only way anything in the buffer stops - that and a patch swap.
//
// **A note already held is not struck again.** It is sounding, and a second
// note-on for it would need a second note-off to match, which a buffer that
// releases by pitch cannot tell apart from the first.
//
// **The reset is read before the notes of the same pass**, so a reset and the
// first note of the next chord arriving together leave that note held: the
// old chord goes, the new one starts.
//
// **A stop does not empty it.** What it holds was played, not clocked; a
// transport stop is no reason to let go of it, and the reset says when to.
//
// When the ledger is full (`MAX_SOUNDING_NOTES`) a new note is dropped, not
// sounded: nothing is emitted that cannot be released (midi/sounding_notes.h).
//
// Inlet 0 (note): note-ons are held, note-offs are swallowed. Anything that is
//         not a note - a CC, a bend, aftertouch - travels straight through.
// Inlet 1 (gate): reset. Each rising edge releases everything held. Optional.
// Outlet 0 (note): the held notes.
//
// params[0] channel   0 keeps the channel each note arrived on
//                     (midi/note_event.h)
class NoteBuffer : public Node{
    public:
        static constexpr uint16_t P_CHANNEL = 0;
        static constexpr uint8_t N_PARAMS = 1;

        static const AlgorithmDescriptor descriptor;

        explicit NoteBuffer(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        uint8_t held_count() const { return held.count(); }
        uint32_t dropped() const { return held.refused(); }

    private:
        BusSet in;
        BusSet out;
        uint8_t channel;         // 0 keeps the source's
        EdgeIn reset_in;
        SoundingNotes held;
};

#endif
