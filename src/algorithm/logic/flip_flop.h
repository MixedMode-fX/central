#ifndef MMMC_ALGORITHM_LOGIC_FLIP_FLOP_H
#define MMMC_ALGORITHM_LOGIC_FLIP_FLOP_H

#include "node/node.h"

// One bit of memory, clocked: the flip-flop, in the five shapes a circuit
// is drawn with.
//
// The gates (gates.h) have no state and GateHold has state but no clock; a
// flip-flop is the piece in between, and it is the piece a digital circuit
// is built from. Every type here is one bit that changes on a clock edge and
// holds between edges, so what is downstream of it sees a level that moved
// exactly when the clock said, whatever the inputs did in the meantime -
// which is what a resync is, and what a divider is, and what a "play this
// on the next bar, not now" button is.
//
//   D        on the edge, Q takes `data`. A gate re-timed to the clock: a
//            pad pressed anywhere in the bar is heard on the beat.
//   D latch  Q follows `data` while the clock is open (high, or low for a
//            falling `edge`) and holds when it closes. The transparent
//            latch, and a gate that can only change during part of the bar.
//   T        on the edge, Q flips if `data` is high - and with nothing on
//            `data` it flips on every edge, which is a divide-by-two whose
//            output is a square rather than a trigger. Chain them for four,
//            eight, sixteen; Counter is that chain in one node.
//   JK       on the edge: J (`data`) sets, K resets, both flip, neither
//            holds. The universal one, so a rule with four cases can be
//            wired rather than patched round.
//   SR       set and reset by level. With a clock patched they are read on
//            the edge; without one, every pass. Reset wins, as it does on
//            GateHold, so the one pass where both are high ends low.
//
// `clear` is the asynchronous reset every real flip-flop has: a rising edge
// drops Q whatever the clock is doing, and wins over the edge in the same
// pass. It is how a transport start puts a whole circuit back to zero.
//
// Both outlets are levels, written every pass, because a bus is cleared by
// the swap and the bit is the node's, not the bus's. `not Q` is there
// because half of what a flip-flop is used for wants the complement, and a
// NOT after it would cost a pass in a loop.
//
// Inlet 0 (gate, optional): data - D, T, J or S by type.
// Inlet 1 (gate, optional): K, or R.
// Inlet 2 (gate, optional): clock.
// Inlet 3 (gate, optional): clear.
// Outlet 0 (gate): Q.
// Outlet 1 (gate): not Q.
//
// params[0] type   D / D latch / T / JK / SR
// params[1] edge   rising / falling: which clock edge, and which level
//                  holds the latch open
//
// Live edits (#20): changing the type keeps the bit. A latch turned into a
// toggle is still high, and the next edge flips it.
class FlipFlop : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Type : uint8_t {
            FF_D       = 1,
            FF_D_LATCH = 2,
            FF_T       = 3,
            FF_JK      = 4,
            FF_SR      = 5,
            FF_TYPES   = 5,
        };
        enum Edge : uint8_t {
            EDGE_RISING  = 1,
            EDGE_FALLING = 2,
            EDGES        = 2,
        };
        static constexpr uint8_t IN_DATA = 0, IN_K = 1, IN_CLOCK = 2, IN_CLEAR = 3;
        static constexpr uint16_t P_TYPE = 0, P_EDGE = 1;

        explicit FlipFlop(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        bool q() const { return bit; }

    private:
        BusSet data_in;
        BusSet k_in;
        BusSet clock_in;
        BusSet clear_in;
        BusSet q_out;
        BusSet nq_out;
        uint8_t type;
        uint8_t edge;
        bool bit;
        bool last_clock;
        bool last_clear;
};

#endif
