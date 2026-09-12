#ifndef MMMC_ALGORITHM_ARPEGGIATOR_H
#define MMMC_ALGORITHM_ARPEGGIATOR_H

#include "node/node.h"
#include "midi/held_notes.h"
#include "midi/sounding_notes.h"
#include "util/random.h"

// Plays the held notes one at a time, one per advance edge.
//
// It has no clock of its own. Rate comes from whatever drives the advance
// inlet, so a ClockDiv sets the tempo, a logic gate or an external jack works
// just as well, and several arpeggiators sharing one divider are locked to
// each other by construction (#4, #9).
//
// **Hold** latches the chord, so the figure keeps running with nobody
// touching the keyboard - which is what an arpeggiator is for on a module
// with no keyboard attached. It is deliberately not a sustain pedal: while
// hold is on, a note-off is remembered but changes nothing, and the *next*
// note-on played after every key has been released replaces the figure
// rather than adding to it. That is the behaviour every hardware arpeggiator
// has, and the reason for it is that adding would make the chord grow by one
// note every time a player fumbles a change. Adding to a held chord is still
// possible: keep one key down and play the rest.
//
// Turning hold off keeps whatever is still physically held and drops the
// rest, so lifting the latch under your fingers does not cut the notes you
// are actually holding.
//
// **New chord** is what a chord change does to the cursor. On *restart* a
// chord played with no key already down starts the figure again at its first
// step, so the first note is the one the mode asks for - the lowest for up,
// the highest for down - however far through the figure the previous chord
// got. On *run on* the cursor is left where it is and only the reset inlet
// moves it, so a chord progression plays as one continuous figure rather
// than as a phrase restarted under every chord. Adding to a chord already
// down never restarts either way: a figure running under a growing chord
// keeps its place.
//
// Inlet 0 (note): the held chord. Note-on adds, note-off removes.
// Inlet 1 (gate): advance. Each rising edge releases the sounding note and
//                 plays the next one.
// Inlet 2 (gate, optional): reset. A rising edge sends the pattern back to
//                 its first step, so another node can restart the figure.
// Inlet 3 (gate, optional): hold. High latches the chord, exactly as the
//                 parameter does - so a footswitch on a jack works, and
//                 either one on its own is enough.
//
// params[0] mode      0 up, 1 down, 2 up-down, 3 random, 4 as-played
// params[1] octaves   1..4 (0 -> 1). The figure repeats an octave higher each
//                     time round; a step that would leave 0..127 is skipped.
// params[2] gate ms   how long each note sounds. 0 holds it until the next
//                     step, which is what a legato arpeggio wants.
// params[3] velocity  0 keeps the velocity each note was played with
// params[4] hold      latch the chord: it keeps playing with no key down
// params[5] new chord 0 restart the figure, 1 run on from where it was
class Arpeggiator : public Node{
    public:
        enum Mode : uint8_t {
            ARP_UP = 0, ARP_DOWN = 1, ARP_UP_DOWN = 2, ARP_RANDOM = 3, ARP_AS_PLAYED = 4,
        };
        static constexpr uint8_t MAX_OCTAVES = 4;

        static const AlgorithmDescriptor descriptor;
        explicit Arpeggiator(const NodeConfig& config);
        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        // A legato step is held until the next advance, and a latched chord
        // has no note-off coming to end it (node/node.h).
        void transport_stopped(BusManager& bus) override { silence(bus); }
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t held_count() const { return held.count(); }
        uint8_t sounding_count() const { return sounding.count(); }
        // Keys physically down, as opposed to notes in the figure: the two
        // differ exactly while the chord is latched, which is what makes
        // this worth asserting on in a test.
        uint8_t keys_down() const { return down_count; }
        bool latched() const { return held.count() != 0 && down_count == 0; }

    private:
        uint8_t steps() const;              // notes held, times the octave range
        void step(BusManager& bus, uint32_t now_us);
        void note_for(uint8_t index, uint8_t& note, uint8_t& velocity, uint8_t& channel) const;
        void release(BusManager& bus);
        // Which keys are physically down. Kept apart from `held` because
        // hold is the state where they disagree, and one bit per note is
        // cheaper than a second HeldNotes and cannot overflow.
        bool key_down(uint8_t note) const;
        void set_key(uint8_t note, bool down);
        // Everything the figure holds that no key is holding.
        void drop_latch(BusManager& bus);

        uint8_t held_in;
        uint8_t advance_in;
        uint8_t reset_in;
        uint8_t hold_in;
        uint8_t out;
        uint8_t mode;
        uint8_t octaves;
        uint16_t gate_ms;
        uint8_t fixed_velocity;
        uint8_t hold;                       // the parameter, not the inlet
        uint8_t run_on;                     // a chord change leaves the cursor
        uint8_t cursor;                     // position in the figure
        uint8_t playing;                    // source key of the sounding note
        uint32_t started_us;
        bool descending;                    // up-down direction
        bool last_advance;
        bool last_reset;
        bool holding;                       // parameter or inlet, last pass
        uint32_t down[4];                   // one bit per MIDI note
        uint8_t down_count;
        HeldNotes held;
        SoundingNotes sounding;
        Xorshift32 rng;
};

#endif
