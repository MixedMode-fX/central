#ifndef MMMC_ALGORITHM_MIDI_NOTE_FILTER_H
#define MMMC_ALGORITHM_MIDI_NOTE_FILTER_H

#include "node/node.h"
#include "midi/sounding_notes.h"

// Decides what passes. One note bus in, one out, and nothing is changed -
// only kept or dropped.
//
// **The note bus is a merge, so the filter is the split.** A bus already
// fans out to every reader and fans in from every writer, which means a
// splitter node would be a second way to say what a bus says already: three
// of these on one bus with different ranges *is* a three-zone keyboard
// split, and the zones are rejoined by pointing them at one output bus. That
// is why this is a filter rather than a router with several outlets.
//
// **Every test is taken on the note-on, and the note-off follows it** -
// Probability's rule, for Probability's reason. A note-off carries velocity
// 0 and would fail any velocity window, and a range or a channel that moved
// under a held note would stop matching it, so a note-off evaluated on its
// own merits is how a filter hangs a synth. Instead the pass is recorded
// (midi/sounding_notes.h) and the release is looked up: a note whose note-on
// was dropped has no record, so its note-off is dropped too, and a note that
// got through is always released however the parameters have moved since.
//
// **`not channel` inverts the channel test and nothing else.** The channel
// is the one field with sixteen values and a single slot to name them in, so
// "everything that is not the drum channel" would otherwise be fifteen of
// these; every other clause already has its own way round. A note window is
// notched by two filters on one bus, and the type test inverts by choosing
// the other answer - `controls` is `notes` inverted. An invert that negated
// the whole test would take that away rather than add to it: there would be
// no way left to say "the notes, but not the ones on channel 10".
//
// A message that is not a note is tested on its channel and its type alone -
// it has no pitch and no velocity to compare - and is written through
// untouched. Re-addressing it is `Channel`'s job, not this node's.
//
// A window whose top is below its bottom passes nothing, and is left to do
// so: clamping the two together would hide the mistake behind notes the user
// did not ask for.
//
// Inlet 0 (note): everything arriving.
// Outlet 0 (note): what passed.
//
// params[0] channel  0 is any channel
// params[1] low      lowest note-on that passes
// params[2] high     highest note-on that passes
// params[3] vel min  softest note-on that passes
// params[4] vel max  loudest note-on that passes
// params[5] pass     which message types are eligible at all
// params[6] not channel  match every channel except `channel`
class NoteFilter : public Node{
    public:
        static constexpr uint16_t P_CHANNEL = 0, P_LOW = 1, P_HIGH = 2,
                                  P_VEL_MIN = 3, P_VEL_MAX = 4, P_PASS = 5,
                                  P_NOT_CHANNEL = 6;
        static constexpr uint8_t N_PARAMS = 7;
        static constexpr uint8_t ANY_CHANNEL = 0;
        static constexpr uint8_t DEFAULT_HIGH = 127, DEFAULT_VEL_MAX = 127,
                                 DEFAULT_VEL_MIN = 1;

        // Which types are eligible. The two answers anybody actually wants
        // are "just the notes" and "everything but the notes": a keyboard
        // sends notes, bend, pressure and its own CCs down one cable, and
        // until now nothing in the module could take one of those away from
        // the other. A finer mask would be a bitfield, which an editor can
        // only draw as eight unnamed bits.
        enum Pass : uint8_t {
            PASS_ALL      = 0,   // notes and everything else
            PASS_NOTES    = 1,   // note-on and note-off only
            PASS_CONTROLS = 2,   // everything that is not a note
            PASS_MODES    = 3,   // one past the last
        };

        static const AlgorithmDescriptor descriptor;
        explicit NoteFilter(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // What the filter says about one event. Only meaningful for a
        // note-on and for messages that are not notes: a note-off is never
        // tested, it is looked up.
        bool passes(const MidiEvent& event) const;

        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }

    private:
        uint8_t in;
        uint8_t out;
        uint8_t channel;
        uint8_t low;
        uint8_t high;
        uint8_t vel_min;
        uint8_t vel_max;
        uint8_t pass;
        bool not_channel;
        SoundingNotes sounding;
};

#endif
