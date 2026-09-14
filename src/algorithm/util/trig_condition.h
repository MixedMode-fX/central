#ifndef MMMC_ALGORITHM_UTIL_TRIG_CONDITION_H
#define MMMC_ALGORITHM_UTIL_TRIG_CONDITION_H

#include <stdint.h>
#include "node/param.h"
#include "util/random.h"

// Whether an event gets to happen. One decision, one implementation, two
// nodes: Probability asks it of a note-on, GateProbability of a rising edge,
// and neither owns a byte of it.
//
// A percentage on its own only ever thins a pattern. What makes a repeating
// pattern *develop* is a condition evaluated against the count of times the
// event has come round - Elektron's trig conditions, which are the reference
// for the vocabulary here. A hat that plays every other bar, a fill that
// arrives on the fourth, a snare that answers whichever note the one before
// it did not: all of those are one event with a rule on it, and none of them
// is reachable with a dice roll.
//
// Three independent controls, ANDed, rather than Elektron's single list.
// There the percentages and the ratios are entries in one menu and a trig
// has exactly one condition; here `chance`, `ratio` and `condition` each do
// one thing and compose, so "half the time, but only on the first of every
// four bars" is a setting rather than a patch. Set two of them to their
// defaults and what is left is the Elektron control of the same name.
//
//   chance     the dice: percent, applied last and only to an event the
//              other two have already let through.
//   ratio      X:Y - of every Y events that reach the node, the Xth passes.
//              Elektron counts pattern loops, which is the same count seen
//              from the trig: the trig comes round once per loop. Counting
//              events is what makes it mean the same thing here, where there
//              is no pattern to loop.
//   condition  first / not first / pre / not pre / nei / not nei / fill /
//              not fill.
//
// **`pre` and `nei` are the same quantity seen from two sides**, which is why
// there is one name for it on the wire: `passed` is an outlet carrying the
// node's last decision, and `nei` is the inlet that reads somebody else's.
// Patch A's `passed` into B's `nei` and B plays only where A did (`nei`) or
// only where A did not (`not nei`) - Elektron's neighbour track, as a cable.
// `pre` is the same test against the node's own last decision. The outlet is
// **latched**, not a pulse: it holds the last decision until the next event,
// so it says what it says whether or not the neighbour is clocked in the
// same pass.
//
// **`fill` is an inlet, not a mode.** Elektron has a fill button; a module
// has a gate bus, so anything can be the button - a jack, a latched GateHold,
// a sequencer lane, a comparator on an LFO.
//
// The count restarts on a transport start, which is what makes `first` and a
// ratio mean the same thing twice running. A continue does not restart it:
// continue is the message that means "where we left off".
class TrigCondition {
    public:
        // Indexed from 1, so a stored 0 still means the descriptor's default
        // (node/param.h).
        enum Condition : uint8_t {
            COND_ALWAYS    = 1,
            COND_FIRST     = 2,
            COND_NOT_FIRST = 3,
            COND_PRE       = 4,
            COND_NOT_PRE   = 5,
            COND_NEI       = 6,
            COND_NOT_NEI   = 7,
            COND_FILL      = 8,
            COND_NOT_FILL  = 9,
            COND_COUNT     = 9,
        };

        // `ratio` is one enum rather than an X and a Y, because half the
        // pairs two bytes can express (X > Y) are not a rhythm, and because
        // the list is what a user reads on the device this borrows from.
        // Index 1 is off; 2.. are 1:2, 2:2, 1:3, 2:3, 3:3, ... 8:8.
        enum : uint8_t {
            RATIO_OFF   = 1,
            RATIO_MAX_Y = 8,
            RATIO_COUNT = 36,       // off, then sum(2..RATIO_MAX_Y) pairs
        };

        // The event counter wraps on the lowest common multiple of every Y a
        // ratio can name, so `position % y` never steps over a discontinuity
        // however long the patch runs.
        static const uint16_t RATIO_CYCLE = 840;

        // chance, ratio, condition, seed - the block both nodes expose, in
        // this order, starting at their parameter 0. Shared as descriptors
        // and not only as behaviour: two copies of a 36-entry option table
        // are two things to get out of step.
        static const uint16_t N_PARAMS = 4;
        static const ParamDescriptor PARAMS[N_PARAMS];

        // `params` is the node's NodeConfig::params, read from index 0.
        explicit TrigCondition(const uint8_t* params);

        // Index within the block. A node whose first parameter is this
        // block's first passes its own index straight through.
        bool set_param(uint16_t index, uint8_t value);
        uint8_t get_param(uint16_t index) const;

        // Decide one event, and remember the answer for `pre`, for a
        // neighbour's `nei`, and for the `passed` outlet.
        bool evaluate(bool fill, bool nei);

        // Transport start: back to the top of the count.
        void restart();

        // The last decision, latched: what the `passed` outlet carries.
        bool passed() const { return last_pass; }

        // Diagnostics / tests.
        uint16_t position() const { return at; }
        static void ratio_pair(uint8_t stored, uint8_t& x, uint8_t& y);

    private:
        uint8_t percent;
        uint8_t ratio;
        uint8_t condition;
        uint8_t seed_offset;
        uint16_t at;            // events since the last restart, mod RATIO_CYCLE
        bool seen;              // any event since the last restart
        bool last_pass;
        Xorshift32 rng;
};

#endif
