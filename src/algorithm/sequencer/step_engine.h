#ifndef MMMC_ALGORITHM_SEQUENCER_STEP_ENGINE_H
#define MMMC_ALGORITHM_SEQUENCER_STEP_ENGINE_H

#include <stdint.h>
#include "config.h"
#include "bus/domain.h"
#include "bus/bus_manager.h"
#include "util/random.h"

// The transport every sequencer shares (#13): a step counter, a length, a
// direction, and the rule for what "the next step" is. It is domain-agnostic
// on purpose. A gate sequencer, a note sequencer and each lane of a drum
// sequencer all run one of these; what differs is only what a step holds and
// which bus it writes, so a gate sequencer and a note sequencer at the same
// length and direction visit steps identically by construction.
//
// It has no clock and no edge detection: the node owning it decides when to
// call advance(), normally from a rising edge on its advance inlet (EdgeIn,
// below), which is how a ClockDiv, a logic gate, an external jack or another
// sequencer's output can all drive it.
//
// Reset means the same thing everywhere: the next advance plays the pattern's
// first step - step 0 going forwards, the last step going backwards - and the
// direction only starts counting from the step after that.
class StepEngine {
    public:
        enum Direction : uint8_t {
            SEQ_FORWARD  = 0,
            SEQ_REVERSE  = 1,
            SEQ_PINGPONG = 2,   // endpoints not repeated: 0 1 2 3 2 1 0 1 ...
            SEQ_RANDOM   = 3,   // any step, uniformly
            SEQ_BROWNIAN = 4,   // a random walk: back one, stay, or forward one
            SEQ_DIRECTIONS,
        };

        StepEngine() : len(1), dir(SEQ_FORWARD), cursor(0), steps(0), at_first(true), descending(false) {}

        // `length` is clamped to 1..MAX_SEQUENCE_LEN (0 -> `fallback`);
        // an unknown direction plays forwards.
        void configure(uint8_t length, uint8_t direction, uint8_t fallback = 1){
            len = length ? length : fallback;
            if (len == 0) len = 1;
            if (len > MAX_SEQUENCE_LEN) len = MAX_SEQUENCE_LEN;
            dir = direction < SEQ_DIRECTIONS ? direction : (uint8_t)SEQ_FORWARD;
            reset();
        }

        // Live length and direction changes (#20). Unlike configure() these
        // leave the cursor and the at-first flag alone: a length that drops
        // below the current position is **clamped on the next advance, not
        // immediately**, because an immediate jump reorders the pattern under
        // a running sequence and a musician hears the sequencer stumble.
        void set_length(uint8_t length, uint8_t fallback = 1){
            len = length ? length : fallback;
            if (len == 0) len = 1;
            if (len > MAX_SEQUENCE_LEN) len = MAX_SEQUENCE_LEN;
        }
        void set_direction(uint8_t direction){
            if (direction < SEQ_DIRECTIONS) dir = direction;
        }

        // The next advance plays the first step.
        void reset(){ at_first = true; descending = false; }

        // Moves to the next step and returns it.
        uint8_t advance(Xorshift32& rng){
            steps++;
            if (at_first){
                at_first = false;
                cursor = (dir == SEQ_REVERSE) ? (uint8_t)(len - 1u) : 0;
                return cursor;
            }
            switch (dir){
                case SEQ_REVERSE:
                    cursor = cursor == 0 ? (uint8_t)(len - 1u) : (uint8_t)(cursor - 1u);
                    break;
                case SEQ_PINGPONG:
                    if (len == 1){ cursor = 0; break; }
                    if (descending){
                        if (cursor == 0){ descending = false; cursor = 1; }
                        else cursor--;
                    } else {
                        if (cursor + 1u >= len){ descending = true; cursor = (uint8_t)(len - 2u); }
                        else cursor++;
                    }
                    break;
                case SEQ_RANDOM:
                    cursor = rng.below(len);
                    break;
                case SEQ_BROWNIAN: {
                    const uint8_t move = rng.below(3);          // 0 back, 1 stay, 2 forward
                    if (move == 0) cursor = cursor == 0 ? (uint8_t)(len - 1u) : (uint8_t)(cursor - 1u);
                    else if (move == 2) cursor = (uint8_t)((cursor + 1u) % len);
                    break;
                }
                default:
                    cursor = (uint8_t)((cursor + 1u) % len);
                    break;
            }
            if (cursor >= len) cursor = (uint8_t)(len - 1u);   // after a live length change
            return cursor;
        }

        uint8_t length() const { return len; }
        uint8_t direction() const { return dir; }
        // The step last returned by advance(); 0 before the first advance.
        uint8_t position() const { return cursor; }
        bool before_first() const { return at_first; }
        uint32_t steps_taken() const { return steps; }

    private:
        uint8_t len;
        uint8_t dir;
        uint8_t cursor;
        uint32_t steps;
        bool at_first;
        bool descending;
};

// A rising-edge detector on one gate bus. NO_BUS never fires, so an optional
// inlet costs nothing to leave unpatched.
class EdgeIn {
    public:
        explicit EdgeIn(uint8_t gate_bus = NO_BUS) : bus(gate_bus), last(false) {}

        bool connected() const { return bus != NO_BUS; }
        // Samples the bus; true on the pass where it went high.
        bool rising(const BusManager& b){
            if (bus == NO_BUS) return false;
            const bool level = b.gate_read(bus);
            const bool edge = level && !last;
            last = level;
            return edge;
        }

    private:
        uint8_t bus;
        bool last;
};

// Per-step probability, as stored in a preset: 0 means "always" so that a
// zeroed block plays every step, otherwise 1..100 percent.
inline uint8_t step_probability(uint8_t stored){
    return stored == 0 ? 100 : (stored > 100 ? 100 : stored);
}

#endif
