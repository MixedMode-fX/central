#ifndef MMMC_ALGORITHM_SWITCH_SELECTOR_H
#define MMMC_ALGORITHM_SWITCH_SELECTOR_H

#include <stdint.h>
#include "bus/domain.h"
#include "bus/bus_manager.h"
#include "algorithm/sequencer/step_engine.h"

// Which position a switch is on, and the three ways of moving it.
//
// The four switches - a gate and a note many-to-one, a gate and a note
// one-to-many - are one mechanism asked of two domains in two directions, so
// the mechanism is written once: a cursor over N positions, moved by a
// `select` parameter, a `step` edge, a `reset` edge, or a control signal.
//
//   select   the position itself, 1-based. A CC, a macro or a modulation
//            route moves it, which is what makes a part a pad can choose.
//            It reads back as the position the switch is *on*, the rule
//            GateHold's `gate` follows: the parameter is the state, so a
//            patch saved on part three loads on part three, and the editor's
//            slider shows the section that is playing.
//   step     a rising edge moves to the next position and wraps at the end
//            of the cycle; the switch is then a sequential switch, and a
//            divider once every four bars is a song structure.
//   reset    a rising edge returns to the first position. Reset wins over a
//            step in the same pass, as it does on every sequencer.
//   CV       when the control inlet is patched it *is* the position, every
//            pass: 0..CV_MAX divided evenly over the cycle, so a Counter's
//            count outlet addresses one part per count and a StepMod steps
//            through them in whatever shape it draws. While it is patched
//            the parameter and the edges are ignored, because two things
//            deciding one position would be a race.
//
//   steps    how long the cycle is. `patched` (the default) is up to the
//            last connected port, so two parts patched into a five-way
//            switch alternate rather than passing three empty slots; a
//            number is exactly that many, and a position with nothing on it
//            is a rest - which is a section too.
//
// A position that the cycle no longer includes is left where it is until the
// next step wraps it back, for the reason StepEngine clamps on the advance
// rather than at once: a switch that jumped when its length was edited would
// change parts under a musician's hand.
class Selector {
    public:
        Selector(BusSet select_cv, BusSet step, BusSet reset,
                 uint8_t n_positions, uint8_t patched,
                 uint8_t select_param, uint8_t steps_stored) :
            cv(select_cv), step_in(step), reset_in(reset),
            n(n_positions ? n_positions : 1), last_patched(patched),
            steps_param(steps_stored), pos(0)
        {
            set_select(select_param);
        }

        // Reads the inlets and moves. The owner compares position() with
        // the position it last acted on, because a parameter write moves
        // the cursor between passes too and is a switch like any other.
        void update(const BusManager& bus){
            if (cv.any()){
                const int32_t v = bus.cv_read(cv);
                const int32_t clamped = v < 0 ? 0 : (v > CV_MAX ? CV_MAX : v);
                pos = (uint8_t)((clamped * cycle()) / CV_FULL);
                if (pos >= n) pos = (uint8_t)(n - 1u);
            } else {
                const bool stepped = step_in.rising(bus);
                const bool reset = reset_in.rising(bus);
                if (reset) pos = 0;
                else if (stepped) pos = (pos + 1u >= cycle()) ? (uint8_t)0 : (uint8_t)(pos + 1u);
            }
        }

        uint8_t position() const { return pos; }            // 0-based
        // The cycle `step` wraps at: `steps` when set, else up to the last
        // patched port, and never less than one.
        uint8_t cycle() const {
            uint8_t c = steps_param ? steps_param : last_patched;
            if (c == 0) c = 1;
            if (c > n) c = n;
            return c;
        }

        // The parameters, as the switches store them.
        bool set_select(uint8_t value){
            if (value == 0) value = 1;
            if (value > n) return false;
            pos = (uint8_t)(value - 1u);
            return true;
        }
        uint8_t select() const { return (uint8_t)(pos + 1u); }
        bool set_steps(uint8_t value){
            if (value > n) return false;
            steps_param = value;
            return true;
        }
        uint8_t steps() const { return steps_param; }

    private:
        BusSet cv;
        EdgeIn step_in;
        EdgeIn reset_in;
        uint8_t n;
        uint8_t last_patched;
        uint8_t steps_param;
        uint8_t pos;
};

// The last connected port, plus one - what `steps: patched` cycles over.
inline uint8_t ports_patched(const BusSet* ports, uint8_t n){
    while (n > 0 && !ports[n - 1].any()) n--;
    return n;
}

// The `steps` option names: "patched", then a count. Indexed from 0, so a
// switch with five positions uses the first six and a router all nine.
extern const char* const SWITCH_STEPS_NAMES[9];

#endif
