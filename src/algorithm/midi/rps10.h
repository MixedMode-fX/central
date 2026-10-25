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
// octave down. The node writes C6 plus the interval.
//
// **`range` is where the interval stops, and it may go past the spec.** The
// manual promises -1..+1 octave, so that is the default; up to MAX_RANGE
// either way is there because what a tracker does outside its stated span
// is something to try on the unit rather than something to rule out here.
//
// **What a note further out than `range` gets is `out of range`.** `clip` gives it
// the nearest shift the range has, so everything past the top is the top.
// `wrap` moves it by as few octaves as bring it inside, so it keeps its
// pitch class: at an octave's range a ninth is a second and two octaves up
// is one, which is how a keyboard wider than the pedal still plays the
// intervals it means. A range under a tritone has intervals no octave can
// bring inside; those are clipped after the fold. Bend is a sweep rather
// than a note, so it always stops at the edge.
//
// **`glide` smooths the tone between shifts**, at a rate rather than in a
// time, as `Slew` does: tens of milliseconds per octave, so a third and an
// octave glide at the same speed. The RPS-10 tracks the sweep, so the shift
// slides instead of stepping. A tone starting from silence starts where it
// is going - there is nothing to glide from.
//
// **The interval is from the root, not from C6.** A note is a shift by how
// far it is from the root: the root itself is unison, a third above it a
// third up, the root an octave down an octave down. The root is the key's
// tonic (midi/global_key.h), in the key's register unless `octave` names
// one - middle C by default, so the octave either side of it is the whole
// range and no one has to play two octaves up to hear the pedal do nothing.
// A patched root inlet outranks the key, as it does on every node that has
// one: its last note-on is the root, in its own register unless `octave`
// names one (global_key::placed), and it holds until the next.
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
// Inlet 1 (note, optional): the root. Note-ons set it; nothing else is read.
// Outlet 0 (CV): the tone pitch (util/tone.h), for a tone jack.
//
// params[0] priority  lowest / highest / latest
// params[1] octave    the root's register: 0 the key's (or the played root's), 1..10 one of its own
// params[2] bend      pitch bend range in semitones (0 = ignore bend)
// params[3] release   hold / unison / off
// params[4] range     the widest shift, in semitones either way (0 = DEFAULT_RANGE)
// params[5] glide     tens of milliseconds per octave (0 = instant)
// params[6] out of range  clip / wrap
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
        enum Edge : uint8_t {
            EDGE_CLIP = 0,
            EDGE_WRAP = 1,
            EDGES     = 1,
        };

        static constexpr uint8_t DEFAULT_RANGE = 12;   // the spec: an octave either way
        static constexpr uint8_t MAX_RANGE = 36;       // C3 .. C9 at the jack
        static constexpr uint8_t NO_ROOT = 0xFF;
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
        // Where the glide is heading: the tone with no glide.
        int16_t target() const { return target_cv; }
        int8_t shift() const { return shift_semis; }
        // The pitch a note is measured from this pass.
        uint8_t root() const;
        bool sounding() const { return tone_cv > 0; }

    private:
        // The glide is carried with sub-bits so a slow one moves at all, as
        // in Slew: a 2.55 s octave is 0.15 tone units a millisecond.
        static constexpr uint8_t SUB_BITS = 8;

        NotePriorityRule rule() const {
            return (NotePriorityRule)(priority - PRIORITY_LOWEST);
        }

        BusSet in;
        BusSet root_in;
        BusSet tone_out;

        uint8_t priority;
        uint8_t octave;         // 0 is the key's register, or the played root's
        uint8_t bend_semis;
        uint8_t release;
        uint8_t range;
        uint8_t glide;
        uint8_t edge;

        uint8_t played_root;    // the root inlet's last note-on, NO_ROOT until one
        uint16_t bend_value;    // 14 bits, centred on BEND_CENTRE
        int8_t shift_semis;     // the held or last shift
        bool has_shift;         // a note has set shift_semis since the patch loaded
        int16_t target_cv;
        int16_t tone_cv;
        int32_t level;          // the gliding tone, in sub-units; 0 while silent
        uint32_t last_us;
        bool have_time;
        HeldNotes held;
};

#endif
