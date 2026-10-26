#ifndef MMMC_ALGORITHM_POCKET_ARP_H
#define MMMC_ALGORITHM_POCKET_ARP_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "util/random.h"

// The Critter & Guitari Pocket Piano's patterns, one per `pattern` option.
//
// **Cascade** is the Pocket Piano's Octave Cascade, ported step for step from
// `chris_shifter()` in C&G's pparp (github.com/critterandguitari/pparp). Every
// advance strikes *every* held note at once, each thrown to its own octave,
// from two above where it was played to one below. Two counters pick the
// octaves: a rotation that runs round four, and a shape that runs round three
// - each voice a different octave, voices paired, all together - so the
// figure repeats every twelve advances, and a chord of four is heard as a
// cascade rather than a chord.
//
// Which octave a note lands on depends on its *voice*, and a voice is a slot,
// not a rank: up to POCKET_VOICES keys take the first free slot in the order
// they go down, keep it until they come up, and any key beyond that is
// ignored, as it is on the instrument. Sorting the chord instead would move
// every voice when one key changed, which is not the sound.
//
// Like Arpeggiator, it has no clock of its own: the advance inlet is the
// tempo, which on the Pocket Piano is a sixteenth note. It plays a figure,
// not a stream, so anything on the chord inlet that is not a note is dropped.
//
// `jitter` is the 201 Pocket Piano's: each note of a strike is late by a
// random 0..jitter ms, so a chord is not four notes pounding on one edge.
//
// Inlet 0 (note): the held chord. Note-on takes a voice, note-off frees it;
//                 what was struck keeps sounding until the next advance.
// Inlet 1 (gate): advance. Each rising edge releases what is sounding and
//                 strikes the next step.
// Inlet 2 (gate, optional): reset. A rising edge sends the figure back to
//                 the step a fresh node starts on.
//
// params[0] pattern   0 cascade
// params[1] gate ms   how long each note sounds. 0 holds it until the next
//                     advance.
// params[2] velocity  0 keeps the velocity each note was played with
// params[3] jitter ms each note of a strike is late by up to this much
// params[4] channel   0 keeps the channel each note arrived on
//                     (midi/note_event.h)
class PocketArp : public Node{
    public:
        static constexpr uint16_t P_PATTERN = 0, P_GATE = 1, P_VELOCITY = 2,
                                  P_JITTER = 3, P_CHANNEL = 4;
        static constexpr uint8_t N_PARAMS = 5;

        enum Pattern : uint8_t { POCKET_CASCADE = 0 };
        static constexpr uint8_t POCKET_VOICES = 4;
        static constexpr uint8_t MAX_JITTER_MS = 50;

        static const AlgorithmDescriptor descriptor;
        explicit PocketArp(const NodeConfig& config);
        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        // A legato strike is held until the next advance (node/node.h).
        void transport_stopped(BusManager& bus) override { silence(bus); }
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // The key in a voice, or NONE.
        uint8_t voice(uint8_t index) const { return index < POCKET_VOICES ? voices[index].note : NONE; }
        // How many octaves below the top a voice is thrown on the step just
        // struck: 0 is two octaves up, 3 is one down.
        uint8_t shift(uint8_t index) const { return index < POCKET_VOICES ? shifts[index] : 0; }
        uint8_t sounding_count() const { return sounding.count(); }

        static constexpr uint8_t NONE = 0xFF;

    private:
        struct Voice {
            uint8_t note;                   // the key, NONE when free
            uint8_t velocity;
            uint8_t channel;
        };
        // One note of a strike, from the edge that struck it until its
        // release: waiting out its jitter, then sounding.
        struct Strike {
            uint8_t source;                 // the key, NONE when idle
            uint8_t note;                   // what is sent
            uint8_t velocity;
            uint8_t channel;
            bool sounding;
            uint32_t at_us;                 // due while waiting, struck while sounding
        };

        void take(uint8_t note, uint8_t velocity, uint8_t channel);
        void free_voice(uint8_t note);
        void advance_cascade();
        void strike(uint32_t now_us);
        void release_strikes(BusManager& bus);

        BusSet held_in;
        BusSet advance_in;
        BusSet reset_in;
        BusSet out;
        uint8_t pattern;
        uint16_t gate_ms;
        uint8_t fixed_velocity;
        uint8_t jitter_ms;
        uint8_t channel;
        uint8_t rotation;                   // pparp's chris_shifter_l, 0..3
        uint8_t shape;                      // pparp's chris_shifter_count2, 0..2
        uint8_t shifts[POCKET_VOICES];
        bool last_advance;
        bool last_reset;
        Voice voices[POCKET_VOICES];
        Strike strikes[POCKET_VOICES];
        SoundingNotes sounding;
        Xorshift32 rng;
};

#endif
