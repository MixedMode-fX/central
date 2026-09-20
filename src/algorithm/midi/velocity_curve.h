#ifndef MMMC_ALGORITHM_VELOCITY_CURVE_H
#define MMMC_ALGORITHM_VELOCITY_CURVE_H

#include "node/node.h"

// Reshapes note-on velocity. Pitch is untouched and no note is ever dropped.
//
// The output is clamped to 1..127 rather than 0..127. A note-on with velocity
// 0 *is* a note-off, so a curve that reached zero would silently turn a note
// into its own release - which looks like a hanging voice that never sounded.
//
// **It keeps a channel per note rather than a SoundingNotes ledger**, and it
// is the only modifier here that does. Every other one decides what to emit
// and can refuse when it has no room to record the release; this one emits
// whatever arrives, so a ledger with a capacity would start dropping notes on
// a node whose whole point is that it never does. What it has to remember is
// only where a note went, which is four bits - so it is a table indexed by
// pitch, it cannot overflow, and nothing it is asked to pass is refused.
//
// Keyed on the note number, as the held-note model is everywhere in this
// module (midi/held_notes.h): two source channels playing one pitch through
// this are one note to it. `NoteFilter` in front of it is how a patch says
// which of the two it meant.
//
// params[0] curve   0 linear, 1 soft (easier to play loud), 2 hard, 3 fixed
// params[1] scale   percent, 0 -> 100
// params[2] offset  signed, applied after the curve and the scale
// params[3] fixed   the value curve 3 sends (0 -> 100)
// params[4] channel 0 keeps the channel each note arrived on
//                   (midi/note_event.h)
class VelocityCurve : public Node{
    public:
        enum Curve : uint8_t { CURVE_LINEAR = 0, CURVE_SOFT = 1, CURVE_HARD = 2, CURVE_FIXED = 3 };

        static constexpr uint16_t P_CURVE = 0, P_SCALE = 1, P_OFFSET = 2, P_FIXED = 3,
                                  P_CHANNEL = 4;
        static constexpr uint8_t N_PARAMS = 5;
        static constexpr uint8_t N_NOTES = 128;

        static const AlgorithmDescriptor descriptor;
        explicit VelocityCurve(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t apply(uint8_t velocity) const;

    private:
        uint8_t in;
        uint8_t out;
        uint8_t curve;
        uint8_t scale;
        int8_t offset;
        uint8_t fixed;
        uint8_t channel;              // 0 keeps the source's
        // Where each pitch's note-on was sent, so its note-off follows it
        // whatever `channel` has become since. 0 for a pitch that is not
        // sounding, which is not a channel and so cannot be mistaken for one.
        uint8_t sent_on[N_NOTES];
};

#endif
