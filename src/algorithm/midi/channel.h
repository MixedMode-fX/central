#ifndef MMMC_ALGORITHM_MIDI_CHANNEL_H
#define MMMC_ALGORITHM_MIDI_CHANNEL_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Says which MIDI channel a stream leaves on. Two parameters, and the second
// is what makes it more than a relabelling.
//
// **`count` is the whole design.** A channel and a count of one puts
// everything on that channel, which is the ordinary case. A count above one
// makes the parameters a *span* of that many channels, wrapping at 16, and
// each note-on is allocated one of them: four mono synths on channels 1 to 4
// become a four-voice polysynth, played from one keyboard, with nothing else
// patched. There is no mode parameter because there is no mode - a span of
// one is a fixed channel, and a user who widens it is not switching the node
// to a different algorithm.
//
// **Allocation avoids a channel that is still sounding.** Blind round-robin
// would hand a note to a channel already holding one while a free channel sat
// idle, which on mono synths is a stolen voice for no reason. So the cursor
// walks the span from where it stopped and takes the first channel this node
// has nothing sounding on; if every one of them is busy it takes the cursor's
// own, because at that point there is no free voice to find and the least
// recently started is the one to reuse.
//
// **The note-off goes out where the note-on did.** It has to: a note
// allocated to channel 3 and released on channel 1 is a note that never
// stops. The ledger (midi/sounding_notes.h) records the channel with the
// note and the release is taken from it, so `channel` and `count` can both
// move under a held chord.
//
// What that ledger is keyed on is the note number, as it is in every modifier
// here - so two source channels playing the *same* note number through one of
// these are one note to it, and releasing either releases both. That is the
// module's held-note model (midi/held_notes.h) rather than this node's
// choice, and `NoteFilter` in front of it is how a patch says which of the
// two it meant.
//
// A message that is not a note has no note to allocate and is re-addressed to
// the first channel of the span. Copying it to all of them would be a
// different decision on every message type - one modulation wheel is not four
// - and this node is not the place to make it.
//
// Inlet 0 (note): the stream to re-address.
// Outlet 0 (note): the same stream, on the channel this node chose.
//
// params[0] channel  the channel, or the first of the span
// params[1] count    how many channels the span covers
class Channel : public Node{
    public:
        static constexpr uint16_t P_CHANNEL = 0, P_COUNT = 1;
        static constexpr uint8_t N_PARAMS = 2;
        static constexpr uint8_t N_MIDI_CHANNELS = 16;

        static const AlgorithmDescriptor descriptor;
        explicit Channel(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // The `slot`th channel of the span, wrapping at 16.
        uint8_t channel_at(uint8_t slot) const;
        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }

    private:
        // The channel the next note-on would take, and the cursor moved past
        // it. Not const: allocating is what advances the round robin.
        uint8_t allocate();
        bool busy(uint8_t midi_channel) const;

        uint8_t in;
        uint8_t out;
        uint8_t first;
        uint8_t count;
        uint8_t next;             // cursor into the span, 0 .. count - 1
        SoundingNotes sounding;
};

#endif
