#ifndef MMMC_CONTROL_MOD_MATRIX_H
#define MMMC_CONTROL_MOD_MATRIX_H

#include <stdint.h>
#include "config.h"
#include "master.h"
#include "patch/patch_manager.h"
#include "control/cc_mapper.h"

// Modulation: a control signal reaching a parameter.
//
// A modulator - an LFO, a sample and hold, anything that writes a CV bus -
// produces a value every pass. A parameter is a byte on a node. This is the
// piece between them, and it is deliberately built the same way CcMapper is,
// because it is answering the same question with a different source:
//
//   * **The table is part of the Patch, not the graph.** A parameter has no
//     domain, no fan-in rule and no per-pass value, so writing one is a
//     control-plane operation. control/cc_mapper.h argues this at length for
//     a controller binding and every word of it applies here.
//   * **It writes through CcMapper::write_control**, which ends at
//     PatchManager::set_param - the same validated entry point the protocol,
//     the console and a mapped CC use. One applier, one validator, one set of
//     tests. A modulator cannot reach anything a knob could not, and is
//     refused identically when it asks for something out of range.
//   * **One write per route per pass**, whatever the signal did in between.
//     A parameter write is not free - #20's rule is that a node re-derives
//     what it cached - so a modulator running at bus rate must not turn into
//     a thousand set_param calls a second per target. The value is read once
//     per pass and written once, and #20's "a set that does not change the
//     value returns early" makes a slow modulation cost almost nothing.
//
// **Where it runs.** Between passes, from the main loop, after CcMapper::apply
// and before MixedModeMaster::pass. The buses' front buffer holds what the
// modulators wrote during the previous pass, so a route reads a value that is
// finished and published rather than one a pass is still accumulating - which
// is also why it does not matter which node wrote it.
//
// **Absolute and offset.** Absolute is the modulator behaving as a knob:
// the signal is the value. Offset keeps the parameter's own setting as a
// centre and swings around it, which is what makes a CC and an LFO on one
// target *cooperate* - the knob moves the centre, the LFO moves around it.
// That only works if the matrix can tell "the user moved the set point" from
// "this is what I wrote last pass", so every offset lane remembers what it
// wrote: a target that is not where the matrix left it has been moved by
// somebody else, and the centre is re-taken from it. That is the same anchor
// discipline CcMapper's scale takeover uses, for the same reason.
//
// **Two routes may not share a target.** The validator refuses it
// (MixedModeMaster::route_valid), because two writers racing over one value
// has no defined result - and because the module already has a place to mix
// two modulators, which is the CV bus itself: fan-in there is a sum.

// Why a route is doing nothing, when it is doing nothing.
//
// A modulation that does not move its target is the hardest thing in this
// module to work out from the outside: the parameter simply sits where it was
// put, and every one of half a dozen different reasons looks exactly like
// that. The matrix decides which one it is once a pass anyway - the early
// returns in apply_one() *are* the reasons - so it says which rather than
// leaving an editor to re-derive them from the patch and get a different
// answer.
//
// Never renumber: these travel on the wire (SYSEX_MOD_STATE), and a host
// reading a status it does not know shows the number rather than a wrong word.
enum ModStatus : uint8_t {
    MOD_STATUS_UNUSED = 0,    // no bus: the slot is empty
    MOD_STATUS_SILENT = 1,    // depth is zero - switched off, not broken
    MOD_STATUS_NO_TARGET = 2, // nothing answers at that target: a stale route
    MOD_STATUS_REFUSED = 3,   // the target would not give or take a value
    MOD_STATUS_PINNED = 4,    // the range it may write is one value wide
    MOD_STATUS_ACTIVE = 5,    // it is writing
};

// What one route is doing, as of the last pass. Everything here is read off
// the same variables apply_one() uses, at the point it uses them, so this
// cannot disagree with what the matrix actually did.
struct ModState {
    uint8_t  status;     // ModStatus
    int16_t  cv;         // the raw bus value this route read
    uint16_t position;   // that signal as this route reads it, 0 .. CV_MAX
    uint16_t range_lo;   // the range the route may write, already clamped
    uint16_t range_hi;   // into the target's own
    uint16_t centre;     // offset mode's set point; range_lo in absolute mode
    uint16_t value;      // where the target is, in its own units
};

class ModMatrix {
    public:
        ModMatrix(PatchManager& patches, CcMapper& mapper);
        ModMatrix(const ModMatrix&) = delete;
        ModMatrix& operator=(const ModMatrix&) = delete;

        // Once per main loop, before the pass. Reads the published CV buses
        // and writes at most one value per route.
        void apply(const BusManager& buses, uint32_t now_us);

        // Forget every centre and every remembered write. Called for you when
        // a route changes under the matrix - see the note on `tracked` below -
        // so this is only for a caller that knows something bigger happened.
        void reset();

        // Diagnostics, as CcMapper's.
        uint32_t writes() const { return write_count; }
        uint32_t refused() const { return refuse_count; }
        // What a route last wrote, in the target's own units. False when the
        // slot is unused or has not written yet - which is what an editor
        // needs to draw a modulated parameter as modulated.
        bool last_written(uint8_t slot, uint16_t& value_out) const;
        // What a route is doing, and why it is doing nothing when it is not.
        // False only for a slot that is not a slot. A route the matrix has
        // never reached - the patch has just loaded - reads as unused, which
        // is what it is until the first pass says otherwise.
        bool state(uint8_t slot, ModState& state_out) const;

    private:
        struct Lane {
            // The route this lane's state belongs to. A route edited under
            // the matrix - a patch load, a slot recall, one field changed
            // from the editor - makes the lane's centre meaningless, and
            // comparing what we are tracking against what the patch now says
            // catches all of those in one place without anybody having to
            // remember to call reset().
            ModRoute tracked;
            uint16_t centre;        // offset mode's set point
            uint16_t written;       // what this lane last wrote
            bool have_written;
            ModState reported;      // what apply_one() saw, for an editor
        };

        // The signal on `bus`, read as this route asks and turned into a
        // position 0 .. CV_MAX (absolute) and a signed swing (offset).
        static void read_signal(const ModRoute& route, int16_t cv,
                                int32_t& position, int32_t& swing);
        void apply_one(uint8_t slot, const BusManager& buses, uint32_t now_us);
        static bool same_route(const ModRoute& a, const ModRoute& b);
        // A lane that has decided nothing yet.
        static ModState idle_state();

        PatchManager& patches;
        CcMapper& cc;
        Lane lanes[N_MOD_ROUTE];
        uint32_t write_count;
        uint32_t refuse_count;
};

#endif
