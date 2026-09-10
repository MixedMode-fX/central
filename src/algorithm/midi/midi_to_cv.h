#ifndef MMMC_ALGORITHM_MIDI_TO_CV_H
#define MMMC_ALGORITHM_MIDI_TO_CV_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "midi/held_notes.h"

// MIDI to CV/gate: a note stream becomes a monophonic voice on the control
// buses - pitch, gate, velocity, modulation and a trigger.
//
// This is the mirror of `GateToNote`, and the node that makes everything
// upstream of it - a keyboard, a sequencer, an arpeggiator, the modifiers -
// reach something that is not a MIDI instrument. It writes CV buses, so the
// pitch it produces is already a control signal the modulation matrix can
// route (keyboard tracking is a route from this node's pitch outlet to a
// parameter, with nothing new in the matrix), and it will be a voltage at a
// jack the moment `CvOutPort` exists. The gate and the trigger are gate
// buses, which do reach a jack today through `GateOutPort`.
//
// **One voice, because a CV pair is one voice.** Pitch is a level and a gate
// is a level: neither can carry a second note, so the node has to choose, and
// which note wins is the same question `NotePriority` answers. It is asked
// here rather than solved by patching a `NotePriority` in front, because the
// answer is not only "which note sounds" - it is also when the gate falls,
// when the trigger fires, and which velocity the voice takes, none of which
// survives a trip through a note bus as anything the next node could read.
//
// **What a number on the pitch bus means.** Full scale (`CV_FULL`) is
// `range` octaves, so `range` is what a 12-bit DAC's span will be in volts
// under the usual 1 V/octave: ten octaves over ten volts is the default and
// puts one semitone at 34.1 bus units. `base` is the note that sits at zero -
// C2 by default, the bottom of a five-octave controller - and notes below it
// clamp there, because a fixed range has a bottom and a silently wrapped
// pitch is worse than a flat one. The arithmetic is carried with eight
// sub-bits so that pitch bend, which is a fraction of a semitone, is not
// rounded away before it reaches the bus.
//
// **Pitch bend is part of the pitch, not a second output.** A converter that
// put bend on its own bus would be asking the patch to add two signals that
// nothing downstream adds - a DAC has one input per jack. `bend` is the
// deflection in semitones at full travel, and 0 ignores bend entirely.
//
// **Gate and trigger are both there because they answer different
// questions.** The gate is up for as long as a note is held, which is what an
// envelope's sustain segment needs; the trigger is a fixed-width pulse on
// every attack, which is what re-strikes an envelope without releasing it.
// With `gate` set to `retrigger` the gate itself also drops for one pass on
// an attack, for envelopes that have no trigger input of their own - and that
// is a choice rather than the default, because a legato line played into a
// re-gating converter loses its legato.
//
// **The sustain pedal is not handled here.** Holding a note after its key is
// released is an operation on the note stream, so it belongs in a modifier
// upstream where every node downstream benefits from it - the same reason
// smoothing lives in `Slew` and not inside the modulation matrix.
//
// Inlet 0 (note, required): the stream to convert.
// Outlet 0 (CV):   pitch
// Outlet 1 (gate): high while a note is held
// Outlet 2 (CV):   velocity of the note the voice is playing, held after release
// Outlet 3 (CV):   the modulation source, held
// Outlet 4 (gate): a fixed-width trigger on every attack
//
// params[0] priority  lowest / highest / latest
// params[1] range     octaves of pitch across full scale
// params[2] base      the note that sits at zero
// params[3] bend      pitch bend range in semitones (0 = ignore bend)
// params[4] gate      legato / retrigger
// params[5] width     trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
// params[6] mod src   cc / pressure
// params[7] mod cc    which controller, when the source is cc
//
// Live edits (#20): all eight move at runtime. Changing `range` or `base`
// moves a held note's pitch immediately, which is what a transposition
// control has to do; changing `mod cc` does not clear the level the previous
// controller left, because dropping a modulation to zero on a parameter edit
// is a click nobody asked for.
class MidiToCv : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // Indexed from 1, so a stored 0 still means the descriptor's default -
        // which here is `latest`, the priority a keyboard player expects, and
        // not the first entry in the list. (`NotePriority` numbers the same
        // three from 0 because its default *is* the first of them.)
        enum Priority : uint8_t {
            PRIORITY_LOWEST  = 1,
            PRIORITY_HIGHEST = 2,
            PRIORITY_LATEST  = 3,
            PRIORITIES       = 3,
        };

        enum GateMode : uint8_t {
            GATE_LEGATO    = 0,   // the gate stays up while anything is held
            GATE_RETRIGGER = 1,   // ... and drops for one pass on every attack
            GATE_MODES     = 1,
        };

        enum ModSource : uint8_t {
            MOD_CC       = 1,     // a control change, chosen by `mod cc`
            MOD_PRESSURE = 2,     // channel aftertouch
            MOD_SOURCES  = 2,
        };

        static constexpr uint8_t MAX_RANGE     = 10;   // octaves across full scale
        static constexpr uint8_t DEFAULT_RANGE = 10;
        static constexpr uint8_t DEFAULT_BASE  = 36;   // C2 at zero
        static constexpr uint8_t MAX_BEND      = 12;   // semitones at full travel
        static constexpr uint8_t DEFAULT_BEND  = 2;
        static constexpr uint8_t DEFAULT_MOD_CC = 1;   // the mod wheel
        static constexpr uint16_t BEND_CENTRE  = 8192; // a 14-bit bend at rest

        explicit MidiToCv(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests: what the node last put on each of its outlets.
        int16_t pitch() const { return pitch_cv; }
        bool gate() const { return gate_level; }
        int16_t velocity() const { return velocity_cv; }
        int16_t modulation() const { return mod_cv; }
        // The note the voice is playing, or HeldNotes::NONE.
        uint8_t voice() const { return playing; }
        uint8_t held_count() const { return held.count(); }

    private:
        // Recomputes `per_semitone` from `range`. Called whenever the range
        // moves, never per pass: a division is not free and the answer only
        // changes when a parameter does.
        void derive();
        // Which held note owns the voice, under the current priority.
        uint8_t winner() const;
        // The velocity a held note arrived with, or 0 if it is not held.
        uint8_t velocity_of(uint8_t note) const;
        // Where a note sits on the pitch bus, bend included, clamped.
        int16_t pitch_of(uint8_t note) const;

        uint8_t in;
        uint8_t pitch_out;
        uint8_t gate_out;
        uint8_t velocity_out;
        uint8_t mod_out;
        uint8_t trig_out;

        uint8_t priority;
        uint8_t range;          // octaves across full scale
        uint8_t base;           // the note at zero
        uint8_t bend_semis;     // 0 ignores bend
        uint8_t gate_mode;
        uint8_t width_param;    // milliseconds; 0 is TRIGGER_WIDTH_US
        uint8_t mod_src;
        uint8_t mod_cc;

        // Bus units per semitone, in 1/256ths. Twelve bits over ten octaves
        // is 34.13 units a semitone, and a semitone is not the smallest
        // interval on this bus - bend is - so the fraction is kept.
        int32_t per_semitone;
        uint16_t bend_value;    // 14 bits, centred on BEND_CENTRE
        uint8_t playing;        // HeldNotes::NONE when nothing is held
        uint8_t last_note;      // the pitch the bus holds; outlives the gate
        int16_t pitch_cv;
        int16_t velocity_cv;
        int16_t mod_cv;
        bool gate_level;
        HeldNotes held;
        TriggerPulse pulse;
};

#endif
