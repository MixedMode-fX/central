#ifndef MMMC_ALGORITHM_MIDI_NOTE_DELAY_H
#define MMMC_ALGORITHM_MIDI_NOTE_DELAY_H

#include "node/node.h"
#include "clock/musical_division.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"
#include "midi/sounding_notes.h"

// A note delay that is also a canon generator: one line becomes an ensemble.
//
// Every node in this module is driven by an edge, and in practice every edge
// in a patch descends from one ClockDiv or Metronome. That is deliberate and
// it is what makes the module tight. It also means a patch has exactly one
// rhythmic surface, and until now nothing could put an event *between* the
// grid lines on purpose and stay musical.
//
// The whole ambient tradition is built on the opposite. Eno's Music for
// Airports is seven tape loops of incommensurable length - 23 1/2 seconds,
// 25 7/8, 29 15/16 - that do not come back into alignment for twenty-seven
// days. Reich's phasing is the same idea with two players. This module could
// already build two sequencers of length 16 and 12, which is polyrhythm *on*
// the grid; it had nothing that left the grid.
//
// So: a note in is re-emitted `repeats` times, each one delay further away,
// each transposed by `interval` **steps of the scale** - so a canon at the
// third stays in key - each `decay` percent quieter, each subject to
// `chance`.
//
// **`spread` is the reason the node exists.** At 0 the repeats are exactly on
// the grid and this is a musical delay. Above 0 each gap is a little longer
// than the one before it, so the echoes of a four-note figure stop lining up
// with each other and with the sequencer that produced them, and the texture
// stops being a rhythm and becomes a cloud. Below 0 the gaps shorten and the
// echoes accelerate into each other, which is a ball settling. It is a
// percentage of the delay rather than a number of milliseconds so that it
// means the same thing at every tempo and in both timing modes.
//
// **Two rates, and only one of them is a guess** - the rule Metronome and Lfo
// already follow. Synced, the delay is a note value counted in subticks, so a
// dotted eighth delay under a 1/16 sequence is exact for ever. Free-running
// it is wall-clock time, which is what an echo that should *not* line up with
// the music needs and a synced one cannot express. An echo already in flight
// keeps the clock it was scheduled on, so changing the mode does not strand
// or stampede what is already out there.
//
// **Transposing needs a root**, because a scale step is only defined relative
// to a tonic: the same interval pattern rooted on A and on C are different
// keys, and a delay that assumed C would put its canon a third away in the
// wrong one. The tonic is the key the module is in (midi/global_key.h), which
// is the only place a scale and a root are named.
//
// **The articulation is the input's, not a setting.** A repeat is released
// exactly as long after its note-on as the source note was held, because the
// note-off is scheduled when the source's note-off arrives, at the same
// distance. There is no `hold` parameter and there should not be: a delay
// that gave every echo the same length would flatten the phrasing of the
// thing it is echoing. The consequence is honest and worth stating - a source
// that never sends a note-off has echoes that never end, exactly as the
// source note never ends. The `clear` inlet and a patch swap release them.
//
// **The pass-through is owned too.** `dry` sends the input on to the outlet,
// and an event this node emitted is an event this node owes a note-off for -
// even one it only copied. Without that, a patch swap releases the *echoes*
// from the table and leaves the copy sounding, because the note-off the node
// upstream emits during its own silence() lands on a bus nothing is reading
// any more. So the copies go through a ledger like every other modifier's,
// which also swallows a note-off for a note that was never passed and applies
// the channel override to the copy as well as to the repeats.
//
// **A repeat that cannot be released is never emitted.** Each pending echo
// holds the pitch and channel it will be released with, decided when it was
// scheduled, so the scale, the interval and the key can all move underneath
// and the note-off still matches the note-on. That is #10's ownership rule,
// kept by a table rather than by SoundingNotes because these notes have to be
// *scheduled* before they sound and a ledger only knows about ones that
// already do. When the table is full the echo is dropped and counted, which
// is the same policy the ledger has under overflow and the only one that
// keeps "no hanging notes" true.
//
// Inlet 0 (note): the line to echo.
// Inlet 1 (gate, optional): clear - drop everything scheduled and release
//         everything sounding. A panic button for a texture that has got away.
// Outlet 0 (note): the input, if `dry` says so, plus its repeats.
//
// params[0]  sync      clock / free
// params[1]  division  a note value, synced only
// params[2]  feel      straight / dotted / triplet, synced only
// params[3]  time      free-running delay, in tens of milliseconds
// params[4]  repeats   1..MAX_REPEATS
// params[5]  interval  signed scale steps added per repeat
// params[6]  decay     velocity percent per repeat
// params[7]  chance    percent that each repeat happens at all
// params[8]  spread    signed percent the gaps grow by, per repeat
// params[9]  channel   0 keeps the source's
// params[10] dry       pass the input through, or emit only the repeats
class NoteDelay : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Sync : uint8_t {
            ND_CLOCK = 1,
            ND_FREE  = 2,
            ND_SYNCS = 2,
        };

        enum Dry : uint8_t {
            ND_PASS = 1,
            ND_MUTE = 2,
            ND_DRYS = 2,
        };

        static constexpr uint8_t MAX_REPEATS = 8;
        // Echoes in flight at once. Eight repeats of three notes held together
        // is 24, which is a chord arpeggiated into a delay - the case that
        // actually fills this - and 24 entries is under 400 bytes of a 640
        // byte pool slot. Past it an echo is dropped and counted rather than
        // emitted without a way to release it.
        static constexpr uint8_t MAX_ECHOES = 24;

        explicit NoteDelay(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void tick(BusManager& bus, uint32_t count) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        uint8_t scheduled() const;      // echoes waiting to sound
        uint8_t sounding_count() const; // echoes that have sounded and not been released
        uint8_t passed_count() const { return passed.count(); }
        uint32_t dropped() const { return drops; }
        // The pitch repeat `k` of `pitch` would be emitted at, or 0xFF when
        // it would leave 0..127. k is 1-based, as the repeats are.
        uint8_t pitch_for(uint8_t pitch, uint8_t k) const;
        // The delay of repeat `k`, in whichever unit the current mode counts.
        uint32_t delay_of(uint8_t k) const;

    private:
        enum State : uint8_t { ECHO_FREE = 0, ECHO_WAITING = 1, ECHO_SOUNDING = 2 };

        struct Echo {
            uint32_t on_at;      // subticks or microseconds, per `by_subtick`
            uint32_t off_at;
            uint8_t pitch;       // what will be sent, and released
            uint8_t velocity;
            uint8_t channel;
            uint8_t source;      // the note that caused it, to match its note-off
            uint8_t repeat;      // 1-based, so its delay can be recomputed
            uint8_t state;
            bool by_subtick;
            bool have_off;
        };

        // One period of the delay, in the unit the current mode counts.
        uint32_t unit_delay() const;
        // True when `at` has arrived on the clock the echo was scheduled on.
        bool due(const Echo& e, uint32_t at, uint32_t now_us) const;
        void schedule(uint8_t source, uint8_t velocity, uint8_t channel, uint32_t now_us);
        void note_off_arrived(uint8_t source, uint32_t now_us);
        void release(BusManager& bus, Echo& e);

        uint8_t in;
        uint8_t out;
        uint8_t sync;
        uint8_t div;
        uint8_t how;
        uint8_t time_param;      // tens of milliseconds
        uint8_t repeats;
        uint8_t interval;        // as stored: 128..255 are -128..-1
        uint8_t decay;
        uint8_t chance;
        uint8_t spread;          // as stored, signed
        uint8_t channel;
        uint8_t dry;
        uint32_t subtick;        // the master clock's count, when synced
        uint32_t drops;
        EdgeIn clear_in;
        Xorshift32 rng;
        SoundingNotes passed;      // the dry copies, so a patch swap releases them
        Echo echoes[MAX_ECHOES];
};

#endif
