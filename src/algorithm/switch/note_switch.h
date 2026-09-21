#ifndef MMMC_ALGORITHM_SWITCH_NOTE_SWITCH_H
#define MMMC_ALGORITHM_SWITCH_NOTE_SWITCH_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/switch/selector.h"

// Many note streams in, one out: the outlet carries the selected inlet.
//
// GateSwitch for notes, and the one that makes a song out of sequencers: a
// verse and a chorus each on their own bus, both running, and the switch
// says which one the synth hears. What moves it is the arrangement -
// Selector's step, reset, level or parameter - and the parts never know.
//
// **The switch owns the note-offs of what it lets through.** A note that
// went out from part one is released by part one's note-off, which the
// switch will not see once it has moved to part two, so every note-on it
// passes is recorded (midi/sounding_notes.h) and moving the switch releases
// the record first. The cut is immediate: a section change on the bar is
// what an arrangement means, and a note held across it belongs to the part
// that is no longer playing. A note already held on the part being switched
// *to* is not heard until it is struck again - the switch passes events, and
// its note-on has been and gone.
//
// Everything that is not a note - a CC, a bend, aftertouch - passes from the
// selected inlet only. `channel` is the override every modifier has
// (midi/note_event.h): 0 leaves each message on the channel it arrived on,
// and the note-off leaves where its note-on went whatever it says by then.
//
// Inlets 0..POSITIONS-1 (note, optional): the parts.
// Inlet POSITIONS (CV, optional): the position, as a level.
// Inlet POSITIONS+1 (gate, optional): step.
// Inlet POSITIONS+2 (gate, optional): reset.
// Outlet 0 (note): the selected part.
//
// params[0] select   1..POSITIONS, the position; reads back as where it is
// params[1] steps    the cycle `step` wraps at (0 = up to the last patched)
// params[2] channel  0 keeps the incoming channel, 1..16 re-addresses
class NoteSwitch : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t POSITIONS = MAX_IN - 3;
        static constexpr uint8_t IN_SELECT = POSITIONS, IN_STEP = POSITIONS + 1, IN_RESET = POSITIONS + 2;
        static constexpr uint16_t P_SELECT = 0, P_STEPS = 1, P_CHANNEL = 2;
        static constexpr uint8_t N_PARAMS = 3;

        explicit NoteSwitch(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t position() const { return sel.position(); }
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        BusSet in[POSITIONS];
        BusSet out;
        Selector sel;
        SoundingNotes sounding;
        uint8_t playing;        // the inlet the ledger's notes came from
        uint8_t channel;        // CHANNEL_FROM_SOURCE or an override
};

// One note stream in, many out: the inlet reaches the selected outlet.
//
// GateRouter for notes. One keyboard, one melody, and the router says which
// synth - which bus, which cable - plays it now: a lead that moves to a
// second voice on the chorus, or a sequencer handed round four channels a
// bar at a time. The note-offs follow the same rule as NoteSwitch's, on the
// outlet they went out of: moving the router releases what it has sounding
// there before anything reaches the next one, so a synth that has been
// switched away from is not left holding a note it will never be told to
// let go of.
//
// Inlet 0 (note, required): what is routed.
// Inlet 1 (CV, optional): the position, as a level.
// Inlet 2 (gate, optional): step.
// Inlet 3 (gate, optional): reset.
// Outlets 0..POSITIONS-1 (note): the destinations.
//
// params[0] select   1..POSITIONS
// params[1] steps    the cycle `step` wraps at (0 = up to the last patched)
// params[2] channel  0 keeps the incoming channel, 1..16 re-addresses
class NoteRouter : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t POSITIONS = MAX_OUT;
        static constexpr uint8_t IN_SIGNAL = 0, IN_SELECT = 1, IN_STEP = 2, IN_RESET = 3;
        static constexpr uint16_t P_SELECT = 0, P_STEPS = 1, P_CHANNEL = 2;
        static constexpr uint8_t N_PARAMS = 3;

        explicit NoteRouter(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t position() const { return sel.position(); }
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        BusSet in;
        BusSet out[POSITIONS];
        Selector sel;
        SoundingNotes sounding;
        uint8_t sounding_on;    // the outlet the ledger's notes went out of
        uint8_t channel;        // CHANNEL_FROM_SOURCE or an override
};

#endif
