#ifndef MMMC_ALGORITHM_CV_TO_NOTE_H
#define MMMC_ALGORITHM_CV_TO_NOTE_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/sequencer/step_engine.h"

// A control signal becomes a melody: the quantiser.
//
// **This is the node that lets the CV bus reach the music.** Until it existed
// nothing anywhere read a CV bus into a note or a gate: Lfo, SampleHold, Slew
// and MidiToCv wrote one, SampleHold and Slew read one back, and the only
// other readers were the modulation matrix and the console. So a control
// signal could move a *parameter* and could do nothing else. It could not
// play a note. The commonest generative patch there is - a slow, complex
// control signal through a quantiser is a melody - was not expressible, and
// a CvInPort reading a jack would have landed in the same closed loop.
//
// MidiToCv (algorithm/midi/midi_to_cv.h) is this node's inverse and the two
// are worth patching together: that one sends a note stream out as pitch,
// gate and velocity, this one brings a control signal back as notes. Between
// them the CV bus is a round trip rather than a one-way street, and a voltage
// can be operated on by every modulator in the module on the way past.
//
// It is not `NoteQuantise`, and the two are worth telling apart. NoteQuantise
// takes notes that already exist and snaps their pitches into a scale. This
// takes a *level* - a signal with a value every pass and no events at all -
// and decides both what to play and when. The names say which is which for
// the same reason `NoteQuantise` is not called `Quantise` (see the README's
// "The key").
//
// **Two ways to turn a level into a pitch, and they are different
// instruments.**
//
//   degree  the range is divided into the scale's *notes*, evenly. A uniform
//           signal picks every tone of the scale equally often, and every
//           value of the signal is a tone - there is no such thing as a
//           passing note to be snapped. This is what a generative patch
//           wants, and it is what the note sequencers already do with a
//           stored degree (midi/scale.h).
//   snap    the range is divided into *semitones* and the result is snapped
//           into the scale, which is what a Eurorack quantiser does. A tone
//           the scale contains has a wider catchment than one it does not, so
//           a uniform signal is no longer uniform - and that is exactly right
//           when the signal is a melody somebody meant, because snapping
//           preserves its shape.
//
// **The root names a pitch, and the key names a pitch class.** `root` is an
// absolute note, because only an absolute note can say which octave the
// melody starts in - the same reason the note sequencers keep a root of their
// own. What follows the module's key is the *pitch class*: with no register
// set the tonic played is the key's root inside the octave `root` names, so a
// patch set to C3 in A minor plays from A3; with one, `root` names the
// register instead, an octave below the default being an octave below the key
// (midi/global_scale.h). The `key` parameter is what opts out of all of it;
// naming a scale changes only which notes are played.
//
// **How the level is read** is the modulation matrix's rule, not a new one:
// bipolar adds half of full scale, so a signal centred on zero uses the whole
// range; unipolar clamps at zero. Bipolar is the default because an Lfo's is.
//
// Inlet 0 (CV): the signal to quantise.
// Inlet 1 (gate, optional): trigger. In `trigger` mode a rising edge is when
//         the level is read; `auto` uses this inlet if it is patched and
//         tracks the signal continuously if it is not.
// Inlet 2 (CV, optional): velocity. Unpatched, the `velocity` parameter.
// Outlet 0 (note): the melody.
//
// params[0] map        degree / snap
// params[1] root       the pitch the bottom of the range sits on
// params[2] range      octaves of travel across full scale
// params[3] scale      0 follows the module's key
// params[4] mode       auto / track / trigger
// params[5] polarity   how the level is read
// params[6] gate       note length in ms; 0 holds until the pitch changes
// params[7] velocity   used when the velocity inlet is unpatched
// params[8] channel
// params[9] key        follow the module's root, or use this node's own
//
// The ledger owns the release, so every one of those nine can move under a
// sounding note - the key changing under it included - and the note-off still
// carries the pitch that was actually sent.
class CvToNote : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // Indexed from 1, so a stored 0 still means the descriptor's default.
        enum Map : uint8_t {
            CVN_DEGREE = 1,
            CVN_SNAP   = 2,
            CVN_MAPS   = 2,
        };

        enum Mode : uint8_t {
            CVN_AUTO    = 1,   // the trigger inlet if it is patched, else track
            CVN_TRACK   = 2,
            CVN_TRIGGER = 3,
            CVN_MODES   = 3,
        };

        enum Polarity : uint8_t {
            CVN_BIPOLAR  = 1,
            CVN_UNIPOLAR = 2,
            CVN_POLARITIES = 2,
        };

        static constexpr uint8_t MAX_RANGE = 8;
        // What `root` holds when nothing has moved it, and so the pitch a
        // followed key's register is measured from.
        static constexpr uint8_t DEFAULT_ROOT = 48;
        static constexpr uint16_t P_KEY = 9;
        static constexpr uint8_t N_PARAMS = 10;

        explicit CvToNote(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        // The pitch that is sounding, or 0xFF when nothing is.
        uint8_t playing() const { return sounding.count() ? sounding.at(0).note : (uint8_t)0xFF; }
        // The pitch a level would produce now, without emitting it. 0xFF when
        // the level maps outside 0..127.
        uint8_t pitch_for(int16_t cv) const;
        // The scale and tonic actually played, after the key is resolved.
        uint16_t active_mask() const;
        uint8_t active_root() const;

    private:
        // The level, moved onto 0 .. CV_MAX the way the matrix reads a signal.
        int32_t level_of(int16_t cv) const;
        // Plays `pitch`, releasing whatever was sounding first.
        void strike(BusManager& bus, uint32_t now_us, uint8_t pitch);

        uint8_t in;
        uint8_t trigger_in;
        uint8_t velocity_in;
        uint8_t out;
        uint8_t map;
        uint8_t root;
        uint8_t range;
        uint8_t scale;
        uint8_t mode;
        uint8_t polarity;
        uint8_t gate_ms;
        uint8_t velocity;
        uint8_t channel;
        uint8_t key;
        EdgeIn trigger;
        uint8_t last_pitch;      // what tracking last struck; 0xFF for nothing
        uint32_t due_us;         // when a timed note is released
        bool timed;              // ... and whether one is pending
        SoundingNotes sounding;
};

#endif
