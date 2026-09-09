#ifndef MMMC_ALGORITHM_CLOCK_DIV_H
#define MMMC_ALGORITHM_CLOCK_DIV_H

#include "node/node.h"
#include "clock/trigger_pulse.h"

// Clock division and multiplication as an ordinary node (#4).
//
// Division is not a property every clocked algorithm carries: it is one
// algorithm, written and tested once, that writes a gate bus. Sequencers and
// the arpeggiator take an edge-triggered gate inlet and run at gate rate, so
// any trigger source drives them - a divider, a logic gate, another
// sequencer, or a jack - and several of them reading one ClockDiv are
// phase-locked by construction because they read the same edge.
//
// Two source modes:
//
//   Tick source (inlet unconnected). The node works from the master subtick
//   count, so division, multiplication, phase and delay are all exact and two
//   dividers cannot drift apart however long they run.
//
//   Gate source (inlet connected). The node counts rising edges, so a divider
//   can divide another divider or the external sync jack. **Multiplication is
//   refused in this mode**: with only past edges to go on, a multiplier has to
//   estimate the input period and extrapolate, which puts the extra pulses in
//   the wrong place whenever the tempo moves - exactly when a musician
//   notices. A multiplying config on a gate-sourced node runs as x1 and says
//   so through multiply_refused(), rather than sounding subtly wrong.
//
// params[0] mode      0 = divide, 1 = multiply
// params[1] amount    1..255 (a multiplier must divide CLOCK_SUBTICK exactly)
// params[2] phase     0..255, as a fraction of the output period: 128 is half
//                     an output step late. Rotates the pattern inside its own
//                     cycle, so it wraps.
// params[3] delay     whole PPQN ticks (gate source: whole input edges) of lag
//                     applied on top of phase. Does not wrap.
// params[4] width     trigger width in milliseconds (0 -> TRIGGER_WIDTH_US)
class ClockDiv : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        explicit ClockDiv(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        void tick(BusManager& bus, uint32_t count) override;

        // Diagnostics / tests
        bool gate_sourced() const { return source_in != NO_BUS; }
        bool multiply_refused() const { return refused; }
        uint32_t period() const { return div_period; }
        uint32_t pulses() const { return pulse_count; }

    private:
        void restart();
        void fire(BusManager& bus);
        // Advances the pattern to `position`, firing if a fire point was
        // reached. Positions are subticks from the tick source and input
        // edges from a gate source; the arithmetic is identical.
        void advance_to(BusManager& bus, uint32_t position);

        uint8_t source_in;
        uint8_t out;
        uint8_t mode;
        uint8_t amount;
        uint32_t div_period;     // positions between two output pulses
        uint32_t offset;         // positions before the first pulse
        uint32_t next_fire;
        uint32_t last_position;
        uint32_t edges;          // rising edges seen, gate source only
        uint32_t pulse_count;
        uint32_t now;            // last timestamp process() saw
        TriggerPulse pulse;
        bool refused;
        bool started;
        bool last_gate;
};

#endif
