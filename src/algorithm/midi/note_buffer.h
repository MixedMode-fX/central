#ifndef MMMC_ALGORITHM_MIDI_NOTE_BUFFER_H
#define MMMC_ALGORITHM_MIDI_NOTE_BUFFER_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Every note that comes in stays on until it is cleared - or, in `sustain`,
// for as long as a gate is held, like a sustain pedal that sends notes.
//
// **latch.** A note-on is sent on at once and then held: its note-off is swallowed, so a
// chord played and let go keeps sounding, and the next note played joins it
// rather than replacing it. A rising edge on `reset` releases everything held,
// which is the only way anything in the buffer stops - that and a patch swap.
//
// **sustain.** Inlet 1 is a pedal. While it is low the buffer is a through:
// a note-on goes out and its note-off follows it. While it is high every
// note-off is swallowed, so what is played under it keeps sounding. When it
// falls, everything whose key is up is released; a key still down sounds on
// until its own note-off, as under a real pedal. Unpatched, the pedal is up.
//
// **A note already held is not struck again.** It is sounding, and a second
// note-on for it would need a second note-off to match, which a buffer that
// releases by pitch cannot tell apart from the first.
//
// **The gate is read before the notes of the same pass**, so a reset and the
// first note of the next chord arriving together leave that note held: the
// old chord goes, the new one starts. In sustain, a pedal falling with a
// note-on leaves that note sounding while its key is down.
//
// **A stop does not empty it.** What it holds was played, not clocked; a
// transport stop is no reason to let go of it, and the gate says when to.
//
// When the ledger is full (`MAX_SOUNDING_NOTES`) a new note is dropped, not
// sounded: nothing is emitted that cannot be released (midi/sounding_notes.h).
//
// Inlet 0 (note): note-ons are held, note-offs are swallowed (in sustain,
//         only while the pedal is down). Anything that is not a note - a CC,
//         a bend, aftertouch - travels straight through.
// Inlet 1 (gate): latch - each rising edge releases everything held;
//         sustain - the pedal, held while high. Optional.
// Outlet 0 (note): the held notes.
//
// params[0] channel   0 keeps the channel each note arrived on
//                     (midi/note_event.h)
// params[1] mode      latch | sustain
class NoteBuffer : public Node{
    public:
        static constexpr uint16_t P_CHANNEL = 0;
        static constexpr uint16_t P_MODE = 1;
        static constexpr uint8_t N_PARAMS = 2;

        static constexpr uint8_t MODE_LATCH = 0;
        static constexpr uint8_t MODE_SUSTAIN = 1;

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
        bool key_down(uint8_t note) const { return (down[note >> 5] >> (note & 31)) & 1u; }
        void set_key(uint8_t note, bool is_down);

        BusSet in;
        BusSet out;
        BusSet gate;
        uint8_t channel;         // 0 keeps the source's
        uint8_t mode;
        bool gate_was;
        uint32_t down[4];        // keys between their note-on and note-off
        SoundingNotes held;
};

#endif
