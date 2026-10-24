#ifndef MMMC_ALGORITHM_MIDI_RPS10_H
#define MMMC_ALGORITHM_MIDI_RPS10_H

#include "node/node.h"
#include "midi/held_notes.h"

// Plays a BOSS RPS-10's pitch shift from MIDI: a note becomes the tone its
// rear-panel keyboard control listens for, at a tone jack (GATE_PORT_TONE).
//
// **What the RPS-10 hears.** The keyboard control tracks the pitch of a
// monophonic signal between C5 (523 Hz) and C7 (2093 Hz), and the interval
// from C6 (1046.5 Hz) is the shift: C6 is unison, E6 a major third up, C5 an
// octave down. That is its whole -1..+1 octave range, so the node writes C6
// plus the interval and clamps there: a note further out has no shift to
// give, and the nearest one is a flat note rather than a wrapped one.
//
// **The interval is from `centre`, not from C6.** A keyboard plays the shift
// from wherever it sits - C4 by default, so middle C is unison and the octave
// either side of it is the whole range - and no one has to play two octaves
// up to hear the pedal do nothing.
//
// **Bend is part of the shift**, `bend` semitones at full travel, so a wheel
// sweeps the pitch between the notes the keyboard picks.
//
// **What happens when the keys come up is a setting**, because the RPS-10
// cannot be told "no note": it tracks whatever it hears. `hold` keeps the
// last shift, which is how a harmony set from a keyboard stays put; `unison`
// goes back to C6; `off` stops the tone, which is what lets the front-panel
// pitch control take over again on a unit that falls back to it. Until the
// first note, `hold` and `unison` both play unison, so the unit hears a
// steady signal from the moment the patch loads.
//
// One voice, chosen by `priority`: the RPS-10 shifts by one interval.
//
// Inlet 0 (note, required): the notes that pick the shift.
// Outlet 0 (CV): the tone pitch (util/tone.h), for a tone jack.
//
// params[0] priority  lowest / highest / latest
// params[1] centre    the note that plays unison
// params[2] bend      pitch bend range in semitones (0 = ignore bend)
// params[3] release   hold / unison / off
class Rps10 : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // As MidiToCv: NotePriorityRule from 1, so a stored 0 is `latest`.
        enum Priority : uint8_t {
            PRIORITY_LOWEST  = NOTE_PRIORITY_LOWEST + 1,
            PRIORITY_HIGHEST = NOTE_PRIORITY_HIGHEST + 1,
            PRIORITY_LATEST  = NOTE_PRIORITY_LATEST + 1,
            PRIORITIES       = PRIORITY_LATEST,
        };

        enum Release : uint8_t {
            RELEASE_HOLD   = 0,
            RELEASE_UNISON = 1,
            RELEASE_OFF    = 2,
            RELEASES       = 2,
        };

        // The keyboard control's span, as MIDI notes (C4 = 60).
        static constexpr uint8_t UNISON_NOTE = 84;     // C6, 1046.5 Hz
        static constexpr uint8_t MAX_SHIFT = 12;       // semitones either way
        static constexpr uint8_t DEFAULT_CENTRE = 60;
        static constexpr uint8_t MAX_BEND = 12;
        static constexpr uint8_t DEFAULT_BEND = 2;
        static constexpr uint16_t BEND_CENTRE = 8192;

        explicit Rps10(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests: what the node last wrote, and the shift it
        // means in semitones (bend aside).
        int16_t tone() const { return tone_cv; }
        int8_t shift() const { return shift_semis; }
        bool sounding() const { return tone_cv > 0; }

    private:
        NotePriorityRule rule() const {
            return (NotePriorityRule)(priority - PRIORITY_LOWEST);
        }

        BusSet in;
        BusSet tone_out;

        uint8_t priority;
        uint8_t centre;
        uint8_t bend_semis;
        uint8_t release;

        uint16_t bend_value;    // 14 bits, centred on BEND_CENTRE
        int8_t shift_semis;     // the held or last shift
        bool has_shift;         // a note has set shift_semis since the patch loaded
        int16_t tone_cv;
        HeldNotes held;
};

#endif
