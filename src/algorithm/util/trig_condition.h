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
// arrives on the fourth, an event that happens once and never again: none of
// those is reachable with a dice roll.
//
// **Only the conditions a cable cannot already make.** A trig condition list
// is the shape it is because a groovebox has fixed tracks, no patch cables
// and one button per idea. This module has a gate bus and a table of logic
// nodes, so most of that list is already sayable and importing it would be
// two ways to write one patch:
//
//   FILL      is `AND(input, the fill gate)`. The fill button is a jack, a
//             latched GateHold, a sequencer lane - anything that writes a
//             gate - and AND is a node.
//   NEI       is `AND(input, the neighbour's passed)`, and NOT NEI the same
//             through a NOT. It exists on a groovebox because track N can
//             only ever read track N-1; here `passed` is an outlet and any
//             node can read it, at any distance, across both domains.
//   PRE       chains several conditional trigs on one track. A node is one
//             trig, not a track, so "the previous conditional trig" is the
//             previous *node*, which is NEI again - the outlet, not a rule.
//
// What is left is what no cable can reach, because it is the node's own
// state: the dice, and a counter of the events this node has seen.
//
//   chance     the dice: percent, applied last and only to an event the
//              condition has already let through.
//   condition  a position in that count. `first` and `not first` fire once
//              and forever-after; `X:Y` passes the Xth of every Y events
//              that reach the node. Elektron counts pattern loops, which is
//              the same count seen from the trig - the trig comes round once
//              per loop. Counting events is what makes it mean the same
//              thing here, where there is no pattern to loop.
//
// One enum and not two, because both answers are read off one counter and a
// user picking "the third of every four" is not also picking "not the first".
//
// **The count is put back by a reset inlet, never by the transport.** Every
// sequencer in this firmware counts edges and is returned to the top by a
// rising edge on `reset`; `Transport`'s start outlet patched there is how a
// patch plays from the top when the DAW does (README). A node that restarted
// itself on a MIDI start would be the one thing in the module that could not
// be put back from the patch, and would ignore the cable everything else
// obeys.
class TrigCondition {
    public:
        // Indexed from 1, so a stored 0 still means the descriptor's default
        // (node/param.h). COND_RATIO .. COND_COUNT are the X:Y entries, in
        // the order the option table names them: 1:2, 2:2, 1:3 ... 8:8.
        enum Condition : uint8_t {
            COND_ALWAYS    = 1,
            COND_FIRST     = 2,
            COND_NOT_FIRST = 3,
            COND_RATIO     = 4,     // the first ratio, "1:2"
            COND_COUNT     = 38,    // the last, "8:8"
        };
        static const uint8_t RATIO_MAX_Y = 8;

        // The event counter wraps on the lowest common multiple of every Y a
        // ratio can name, so `position % y` never steps over a discontinuity
        // however long the patch runs.
        static const uint16_t RATIO_CYCLE = 840;

        // chance, condition, seed - the block both nodes expose, in this
        // order, starting at their parameter 0. Shared as descriptors and
        // not only as behaviour: two copies of a 38-entry option table are
        // two things to get out of step.
        static const uint16_t N_PARAMS = 3;
        static const ParamDescriptor PARAMS[N_PARAMS];

        // `params` is the node's NodeConfig::params, read from index 0.
        explicit TrigCondition(const uint8_t* params);

        // Index within the block. A node whose first parameter is this
        // block's first passes its own index straight through.
        bool set_param(uint16_t index, uint8_t value);
        uint8_t get_param(uint16_t index) const;

        // Decide one event, and remember the answer for the `passed` outlet.
        bool evaluate();

        // A rising edge on `reset`: back to the top of the count.
        void reset();

        // The last decision, latched: what the `passed` outlet carries.
        bool passed() const { return last_pass; }

        // Diagnostics / tests.
        uint16_t position() const { return at; }
        // X and Y for a stored condition. Anything that is not a ratio is
        // 1:1, which passes everything, so the caller does not special-case.
        static void ratio_pair(uint8_t stored, uint8_t& x, uint8_t& y);

    private:
        uint8_t percent;
        uint8_t condition;
        uint8_t seed_offset;
        uint16_t at;            // events since the last reset, mod RATIO_CYCLE
        bool seen;              // any event since the last reset
        bool last_pass;
        Xorshift32 rng;
};

#endif
