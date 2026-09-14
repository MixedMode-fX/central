#ifndef MMMC_CONTROL_CONTROL_SUM_H
#define MMMC_CONTROL_CONTROL_SUM_H

#include <stdint.h>
#include "config.h"

class CcMapper;

// Where every offset contribution on a parameter meets.
//
// A macro destination and a modulation route are both asking the same
// question - "push this target by this much, away from wherever it is set" -
// and before macros existed there was only ever one asker, so each route
// computed its own answer and wrote it. Two writers racing over one value has
// no defined result, which is why MixedModeMaster::route_valid refused a
// second route on a target at all.
//
// Macros make that refusal untenable: a destination that rises and then falls
// back is built from two windows on one parameter (node/patch.h), so a
// parameter must be able to take more than one contribution. Once it can, a
// route is just another contributor and refusing *it* would be an
// inconsistency rather than a discipline. So the contributions are collected,
// summed, and written once:
//
//   * **One write per target per pass**, not one per contributor. The rule
//     control/mod_matrix.h states for a route now holds for the target.
//   * **Clipped once, after the sum.** Clamping each contribution into the
//     target's range separately and then adding them would let two
//     contributions that individually fit add up to something that does not,
//     and would quietly change what a sum means near the ends.
//   * **One anchor per target, not per contributor.** Offset means "away from
//     where this is set", and with several contributors there is only one
//     "where this is set" to be away from. A target that is not where this
//     sum left it has been moved by something else - a knob, the editor, a
//     recall - and that new value is the anchor from here on. Re-reading it
//     unconditionally would compound, and the modulation would walk the
//     parameter away in whichever direction it was pushing. That is the same
//     discipline the matrix used per lane, moved up to where the decision now
//     belongs.
//
// A target nobody asks for this pass is dropped, keeping whatever was last
// written to it, and re-anchors from the current value when somebody asks
// again. Absolute-mode routes do not come here at all: absolute means the
// signal replaces the value rather than moving it, so there is nothing to sum.
class ControlSum {
    public:
        // One contributor can name at most one target, so this cannot
        // overflow in practice; the bound is what makes the gather a fixed
        // scratch array rather than a table over N_NODE x N_PARAM.
        static constexpr uint8_t CAPACITY = N_MACRO_DEST + N_MOD_ROUTE;

        explicit ControlSum(CcMapper& mapper);
        ControlSum(const ControlSum&) = delete;
        ControlSum& operator=(const ControlSum&) = delete;

        // Start a pass. Contributions are cleared; anchors are not.
        void begin();
        // Ask for `offset` on a target, in the target's own units. Called by
        // Macros::expand and by the modulation matrix's offset routes.
        void add(uint8_t kind, uint8_t index, uint16_t param, int32_t offset);
        // Sum, clip once, write once.
        void commit(uint32_t now_us);
        // The patch changed under us: forget every anchor.
        void reset();

        // What happened to a target on the last commit, for an editor that
        // wants to draw a parameter as modulated. False when nothing is
        // asking for it.
        bool lookup(uint8_t kind, uint8_t index, uint16_t param,
                    uint16_t& anchor_out, uint16_t& value_out, bool& clipped_out) const;

        uint32_t writes() const { return write_count; }
        uint32_t refused() const { return refuse_count; }

    private:
        struct Target {
            uint8_t  kind;
            uint8_t  index;
            uint16_t param;
            int32_t  offset;       // summed over this pass
            uint16_t anchor;       // where the target sits with nothing asking
            uint16_t value;        // what the sum came to, after clipping
            uint16_t written;      // what was last written here
            bool have_written;
            bool asked;            // somebody contributed this pass
            bool clipped;          // the sum did not fit the target's range
        };

        int16_t find(uint8_t kind, uint8_t index, uint16_t param) const;
        void commit_one(Target& t, uint32_t now_us);
        // Drop the targets nobody asked for, so a removed route stops
        // holding an anchor that no longer means anything.
        void compact();

        CcMapper& cc;
        Target targets[CAPACITY];
        uint8_t n_targets;
        uint32_t write_count;
        uint32_t refuse_count;
};

#endif
