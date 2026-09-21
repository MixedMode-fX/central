#ifndef MMMC_ALGORITHM_LOGIC_SHIFT_REGISTER_H
#define MMMC_ALGORITHM_LOGIC_SHIFT_REGISTER_H

#include "node/node.h"
#include "algorithm/sequencer/step_engine.h"

// A serial-in, parallel-out shift register: eight bits in a row, and on
// every clock edge each takes the one before it and the first takes `data`.
//
// Tap k is therefore `data` as it was k clocks ago, which makes the node a
// delay line for gates measured in clocks rather than milliseconds - a
// figure played on tap 1 is played again on tap 3 two steps later, and a
// drum voice on each tap is a canon. Turing is this register with its own
// noise fed back and a level read off it; this one is the register on its
// own, taking what the patch gives it.
//
// `loop` closes it: what falls off the end at `length` comes back in, OR'd
// with `data`, so a burst of gates played into it once goes round for ever
// and every further gate is added to the loop. A pattern is written by
// playing, which no sequencer here can do for a gate, and `clear` is the
// eraser. The taps past `length` still shift - they are the loop's history
// - and the loop reads the bit at `length`, so shortening it drops the
// oldest steps on the next clock rather than at once.
//
// Inlet 0 (gate, required): clock.
// Inlet 1 (gate, optional): data, sampled on the clock edge.
// Inlet 2 (gate, optional): clear. A rising edge empties every bit.
// Outlets 0..TAPS-1 (gate): the bits, oldest last, as levels.
//
// params[0] length  1..TAPS, where the loop closes (0 -> TAPS)
// params[1] loop    feed the bit at `length` back into the input
class ShiftRegister : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        static constexpr uint8_t TAPS = MAX_OUT;
        static constexpr uint8_t IN_CLOCK = 0, IN_DATA = 1, IN_CLEAR = 2;
        static constexpr uint16_t P_LENGTH = 0, P_LOOP = 1;

        explicit ShiftRegister(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t bits() const { return reg; }

    private:
        EdgeIn clock_in;
        BusSet data_in;
        EdgeIn clear_in;
        BusSet out[TAPS];
        uint8_t length;
        bool loop;
        uint8_t reg;        // bit 0 is tap 1, the newest
};

#endif
