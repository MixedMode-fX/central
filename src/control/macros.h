#ifndef MMMC_CONTROL_MACROS_H
#define MMMC_CONTROL_MACROS_H

#include <stdint.h>
#include "config.h"
#include "node/patch.h"

class ControlSum;

// Macros: one performance control moving several parameters at once.
//
// The table is in the Patch (node/patch.h) and this holds the part that is
// not: where each macro currently is, and whether it has been touched at all.
//
// **A macro is a target, not a source.** Nothing here reads MIDI. A knob
// reaches a macro through CcMapper, because CC_TARGET_MACRO puts it in the
// same target space a node parameter is in, and a CV bus reaches one through
// the modulation matrix for the same reason. So takeover, the three relative
// encodings and 14-bit pairing are implemented exactly once, in the place
// that already implements them, and a macro pot behaves like every other pot
// on the desk.
//
// **A macro holds; it does not store.** The position below is deliberately
// absent from MacroDef: a macro is a performance control, not a value the
// patch reproduces. That leaves a question at load time - a macro with no
// stored position has no position - and `engaged` is the answer. An untouched
// macro contributes nothing at all, so loading a patch sounds like the patch
// rather than like whatever position zero happens to mean. The first movement
// engages it, and from then until the next patch load it holds its
// destinations every pass. It is the same idea as CcMapper's pickup takeover,
// for the same reason: a control whose position means nothing yet must not
// assert it.
//
// **It does not write.** expand() only says what each destination is asking
// for; ControlSum decides what the target ends up at, because a parameter may
// be asked for by several destinations and by the modulation matrix at once
// and the sum is clipped once, not once per contributor.
class Macros {
    public:
        Macros();
        Macros(const Macros&) = delete;
        Macros& operator=(const Macros&) = delete;

        // The patch changed under us: every macro forgets where it was, and
        // is silent again until somebody moves it. A position belonging to a
        // destination list that no longer exists is not a position.
        void reset();

        // Move a macro. Returns false for an index that is not a macro. The
        // first call engages it - see the note above on why that matters.
        bool set(uint8_t index, uint8_t value);
        uint8_t value(uint8_t index) const;
        bool engaged(uint8_t index) const;

        // Offer every engaged macro's destinations to the sum. Nothing is
        // written here.
        void expand(const Patch& patch, ControlSum& sum) const;

        // What one destination asks for at macro position `pos`, as a signed
        // offset in the target's own units.
        //
        // Below src_lo: nothing. Above src_hi: the whole of depth, held. In
        // between: linear. A window with no width (src_hi <= src_lo) is a
        // step at src_lo rather than a division by zero - which is a useful
        // destination in its own right, and the honest reading of "acts from
        // here on".
        static int32_t contribution(const MacroDest& dest, uint8_t pos);

    private:
        uint8_t pos[N_MACRO];
        bool on[N_MACRO];
};

#endif
