#ifndef MMMC_ALGORITHM_MIDI_RETRIGGER_H
#define MMMC_ALGORITHM_MIDI_RETRIGGER_H

#include "node/node.h"
#include "clock/musical_division.h"
#include "algorithm/sequencer/step_engine.h"
#include "midi/held_notes.h"
#include "midi/sounding_notes.h"

// Turns a held chord into a rhythm: every trigger edge re-strikes it.
//
// A drone is not a part. A chord held on a keyboard, or the one a `Chord`
// node plays itself, is a pad - one note-on, one note-off, and nothing in
// between for the rest of the patch to play against. Everything this module
// had for putting a rhythm under it either supplied the notes as well (a
// sequencer, which is then the part rather than an accompaniment of one) or
// took the chord apart (`Arpeggiator`, one note per edge). Nothing re-struck
// the whole chord, which is the rhythm guitar, the horn section and the
// house piano stab - the figure where the harmony is given and the *rhythm*
// is the idea.
//
// So: hold a note or a chord on the note inlet and it does nothing at all.
// Every rising edge on the trigger inlet releases whatever this node has
// sounding and sends the whole held chord again, so the pattern driving the
// trigger becomes the rhythm and the keyboard decides only the harmony.
// `Arpeggiator` is the same input and the same edge spread across the notes;
// this is all of them together.
//
// **The rhythm is somebody else's, which is why there is no rate here.** A
// Metronome, a ClockDiv, a Euclid, an Automaton lane or a jack all drive the
// trigger, so the figure is whatever the patch already generates and two
// Retriggers on one divider are locked to each other by construction (#4,
// #9). A node with its own rate would be a fifth clock to keep in step.
//
// **The length is a note value, not the gap.** A strike lasts `length` at
// `feel` - a 1/16 stab under a 1/4 trigger is staccato, the same stab under
// a 1/16 trigger is legato and the next edge cuts it - which is how an
// articulation is written down, and it is measured in subticks so it is
// exact at every tempo (clock/musical_division.h). `release: tie` gives up
// the length and holds each strike until the next trigger instead: the
// chord never stops sounding and the trigger only re-articulates it, which
// is a pumped pad rather than a stab. A length counts subticks, so a strike
// under a stopped clock is released by transport_stopped() rather than left
// hanging for the length of the stop.
//
// **A note-on is never heard on its own.** Adding a note to the chord does
// not sound it and does not interrupt the strike in progress: it joins at
// the next edge, because a note that spoke the moment it was played would be
// a rhythm with the player's timing in it, and the whole point is that the
// trigger owns the timing. A note-off is immediate, though - lifting a key
// stops that note, the way lifting it stops anything else.
//
// **There is no hold.** `Arpeggiator` latches its chord because a module
// with no keyboard has nothing else to hold one; here the thing that holds
// the chord is whatever is patched in - a `Chord` with nothing on its inlet
// plays the tonic of the key for ever - and a second copy of that state
// machine is a second place for it to be wrong.
//
// Inlet 0 (note): the chord. Note-on adds, note-off removes and releases.
// Inlet 1 (gate): trigger. Each rising edge re-strikes everything held.
// Outlet 0 (note): the strikes. Nothing else the inlet carries is delayed:
//         a CC, a bend or an aftertouch message travels straight through,
//         because a gater of notes is not a gater of the wheel.
//
// params[0] length    the note value a strike lasts
// params[1] feel      straight, dotted (x3/2) or triplet (x2/3)
// params[2] release   `length`, or `tie` to the next trigger
// params[3] velocity  0 keeps the velocity each note was played with
// params[4] channel   0 keeps the channel each note arrived on
//                     (midi/note_event.h)
class Retrigger : public Node{
    public:
        static constexpr uint16_t P_LENGTH = 0, P_FEEL = 1, P_RELEASE = 2,
                                  P_VELOCITY = 3, P_CHANNEL = 4;
        static constexpr uint8_t N_PARAMS = 5;

        static const AlgorithmDescriptor descriptor;

        enum Release : uint8_t {
            RT_LENGTH   = 1,
            RT_TIE      = 2,
            RT_RELEASES = 2,
        };

        explicit Retrigger(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void tick(BusManager& bus, uint32_t count) override;
        void silence(BusManager& bus) override;
        // A tied strike is held until the next trigger and a timed one is
        // waiting on a subtick: on a stopped clock neither arrives, and the
        // note-offs of a chord still held on the inlet do not come either
        // (node/node.h).
        void transport_stopped(BusManager& bus) override { release_all(bus); }
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        uint8_t held_count() const { return held.count(); }
        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t strikes() const { return struck; }
        // Subticks a strike lasts, at the current length and feel.
        uint32_t length_subticks() const { return division_subticks(length, how); }

    private:
        // Releases the chord and sends it again. Pitch order, so the voices
        // of a chord always leave in the same order.
        void strike(BusManager& bus);
        void release_all(BusManager& bus);

        uint8_t in;
        uint8_t out;
        uint8_t length;          // MusicalDivision
        uint8_t how;             // MusicalFeel
        uint8_t release;         // Release
        uint8_t fixed_velocity;  // 0 keeps what was played
        uint8_t channel;         // 0 keeps the source's
        uint32_t subtick;        // the master clock's count
        uint32_t off_at;         // subtick the current strike is released at
        uint32_t struck;
        EdgeIn trigger_in;
        HeldNotes held;
        SoundingNotes sounding;
};

#endif
