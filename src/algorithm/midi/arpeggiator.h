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
// Inlet 0 (note): the held chord. Note-on adds, note-off removes.
// Inlet 1 (gate): advance. Each rising edge releases the sounding note and
//                 plays the next one.
// Inlet 2 (gate, optional): reset. A rising edge sends the pattern back to
//                 its first step, so another node can restart the figure.
//
// params[0] mode      0 up, 1 down, 2 up-down, 3 random, 4 as-played
// params[1] octaves   1..4 (0 -> 1). The figure repeats an octave higher each
//                     time round; a step that would leave 0..127 is skipped.
// params[2] gate ms   how long each note sounds. 0 holds it until the next
//                     step, which is what a legato arpeggio wants.
// params[3] velocity  0 keeps the velocity each note was played with
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

        uint8_t held_count() const { return held.count(); }
        uint8_t sounding_count() const { return sounding.count(); }

    private:
        uint8_t steps() const;              // notes held, times the octave range
        void step(BusManager& bus, uint32_t now_us);
        void note_for(uint8_t index, uint8_t& note, uint8_t& velocity, uint8_t& channel) const;
        void release(BusManager& bus);

        uint8_t held_in;
        uint8_t advance_in;
        uint8_t reset_in;
        uint8_t out;
        uint8_t mode;
        uint8_t octaves;
        uint16_t gate_ms;
        uint8_t fixed_velocity;
        uint8_t cursor;                     // position in the figure
        uint8_t playing;                    // source key of the sounding note
        uint32_t started_us;
        bool descending;                    // up-down direction
        bool last_advance;
        bool last_reset;
        HeldNotes held;
        SoundingNotes sounding;
        Xorshift32 rng;
};

#endif
