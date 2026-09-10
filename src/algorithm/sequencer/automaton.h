#ifndef MMMC_ALGORITHM_SEQUENCER_AUTOMATON_H
#define MMMC_ALGORITHM_SEQUENCER_AUTOMATON_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "algorithm/sequencer/step_engine.h"
#include "util/random.h"

// A one-dimensional cellular automaton driving eight gate lanes: rhythm that
// is neither periodic nor random.
//
// The rhythm family was at one end of the axis or the other. StepSequencer
// and the drum grids play what somebody typed in. EuclidianSequencer plays a
// formula - perfectly even, and therefore perfectly predictable once you have
// heard a cycle. RandomSequencer has no memory. Nothing was in the middle: a
// pattern with structure, that repeats motifs, and never quite repeats
// itself.
//
// A Wolfram elementary rule lives exactly there, and costs a byte. Each cell
// looks at itself and its two neighbours - eight possible neighbourhoods, one
// bit of answer each, so the whole rule *is* an eight-bit number:
//
//     next[i] = (rule >> ((left << 2) | (self << 1) | right)) & 1
//
// Rule 90 is left XOR right, which draws Sierpinski triangles: sparse,
// self-similar, and unreasonably musical. Rule 110 is the famous one, stable
// in places and chaotic in others, and Turing complete. Rule 30 is noise.
// Rule 150 is dense and symmetric. None of them is a pattern anybody typed
// and none of them is a coin flip.
//
// **The lanes are neighbours, and that is the point.** Eight
// EuclidianSequencers give eight patterns that have nothing to do with each
// other. Here what happens on lane 3 propagates to lane 4 on the next step,
// because that is what the rule does - so a kick figure moves into the snare
// and out into the hats, and the eight lanes are a rhythm section rather than
// eight sequencers in a rack.
//
// **What the mathematics does not give you is a way back.** An automaton that
// reaches a fixed point stays there for ever, and a drum machine that stops
// is a bug however correct the rule is. So `revive` watches for the row that
// does not change - which is all-empty under most rules, all-full under some,
// and any other stable configuration - and reloads the seed when it sees one.
// A row that merely blinks between two states is not a fixed point and is
// left alone, because blinking is a rhythm.
//
// `edges` is what makes it a loop or a decay. On a **ring** cell 0's left
// neighbour is the last cell, so activity that runs off one end arrives at
// the other and the pattern sustains. With **dead** edges the row is bounded
// by silence, activity spreads outward and falls off, and the node is a
// one-shot that decays - which is what `revive` then turns back on.
//
// There is no CV outlet. MAX_OUT is eight and the eight lanes are the whole
// point; a *level* that mutates while it repeats is what Turing is for
// (algorithm/modulator/turing.h), and one of each is a better patch than one
// node doing both badly.
//
// Inlet 0 (gate): advance - one generation per rising edge.
// Inlet 1 (gate, optional): reseed - reload `seed` now.
// Outlet 0..7 (gate): one lane per cell. An outlet left unconnected is a cell
//         that still lives and still feeds its neighbours - which is how a
//         three-lane drum part gets its variation from cells nobody hears.
//
// params[0] rule         0..255, the Wolfram rule
// params[1] seed         the row `reseed` and `revive` load
// params[2] edges        ring / dead
// params[3] revive       off / on
// params[4] cells        how many of the eight are in the ring
// params[5] probability  percent chance a live cell reaches its outlet
// params[6] width        trigger width in ms (0 -> TRIGGER_WIDTH_US)
class Automaton : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        enum Edges : uint8_t {
            CA_RING = 1,    // cell 0's left neighbour is the last cell
            CA_DEAD = 2,    // the row is bounded by silence
            CA_EDGES = 2,
        };

        enum Revive : uint8_t {
            CA_REVIVE_OFF = 1,
            CA_REVIVE_ON  = 2,
            CA_REVIVES    = 2,
        };

        static constexpr uint8_t LANES = DRUM_SEQ_LANES;
        static constexpr uint8_t MIN_CELLS = 2;
        // Rule 90 - left XOR right - from a single cell in the middle. The
        // Sierpinski seed, and the reason the node is worth having.
        static constexpr uint8_t DEFAULT_RULE = 90;
        static constexpr uint8_t DEFAULT_SEED = 1u << 3;

        explicit Automaton(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests.
        // The row, cell i in bit i.
        uint8_t row() const { return cells; }
        uint32_t generations() const { return count; }
        uint32_t revivals() const { return revived; }
        // The next row, without stepping onto it.
        uint8_t next_row() const;

    private:
        void reseed();

        EdgeIn advance_in;
        EdgeIn reseed_in;
        uint8_t out[LANES];
        uint8_t rule;
        uint8_t seed;
        uint8_t edges;
        uint8_t revive;
        uint8_t n_cells;
        uint8_t chance;
        uint8_t width_param;
        uint8_t cells;
        uint8_t fired;            // which lanes the current pulse belongs to
        uint32_t count;
        uint32_t revived;
        Xorshift32 rng;
        TriggerPulse pulse;
};

#endif
