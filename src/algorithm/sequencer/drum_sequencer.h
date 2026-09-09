#ifndef MMMC_ALGORITHM_SEQUENCER_DRUM_SEQUENCER_H
#define MMMC_ALGORITHM_SEQUENCER_DRUM_SEQUENCER_H

#include "node/node.h"
#include "clock/trigger_pulse.h"
#include "midi/sounding_notes.h"
#include "algorithm/sequencer/step_engine.h"

// Drum sequencers (#14): a grid of DRUM_SEQ_LANES lanes x MAX_SEQUENCE_LEN
// steps, edited as one object, on one step engine per lane.
//
// One algorithm rather than eight gate sequencers because a user sees one
// instrument, and because eight lanes as eight nodes would be eight pool
// slots plus eight converters. Each lane has its **own length**, which is
// polyrhythm for free: lanes of 16 and 12 steps realign after 48 advances
// and not before. Advance and reset are shared: a reset returns every lane
// to its first step whatever its length.
//
// Two algorithms, not one with a mode switch, because the destinations want
// different things. To gates, velocity has no representation, so accent is
// a second lane in the traditional modular way, and the node has one gate
// outlet per lane. To MIDI, velocity is where the music is, so the grid
// stores one velocity per cell and the node has one note outlet with a note
// number and channel per lane. Neither carries an outlet it never uses, and
// the gate variant stores no velocity, which is why it is a third the size.
//
// Inlet 0 (gate): advance.   Inlet 1 (gate, optional): reset.
//
// Header, params[0..15]:
//   [0] length      default lane length, 1..MAX_SEQUENCE_LEN (0 -> 16)
//   [1] direction   StepEngine::Direction, shared by every lane
//   [2] gate        DrumSeqGate: trigger width in ms (0 -> TRIGGER_WIDTH_US)
//                   DrumSeqMidi: note length in ms (0 -> 10)
//   [3..15]         reserved, zero
// Lanes, from params[LANE_BASE], LANE_STRIDE bytes each - see each class.
//
// Probability is per lane rather than per cell: a per-cell byte would double
// the MIDI variant's grid, and a lane-wide "ghost" percentage is what the
// sizing in #14 budgets for. A cell that wants its own odds is a lane.
class DrumSequencer : public Node{
    public:
        static constexpr uint8_t LANES = DRUM_SEQ_LANES;
        static constexpr uint8_t P_LENGTH = 0, P_DIRECTION = 1, P_GATE = 2;
        static constexpr uint16_t LANE_BASE = 16;
        static constexpr uint8_t LANE_STRIDE = 8;
        static constexpr uint8_t DEFAULT_LENGTH = 16;

        DrumSequencer(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;

        uint8_t lane_length(uint8_t lane) const { return lane < LANES ? lanes[lane].length() : 0; }
        uint8_t lane_position(uint8_t lane) const { return lane < LANES ? lanes[lane].position() : 0; }
        uint8_t lane_probability(uint8_t lane) const { return lane < LANES ? chance[lane] : 0; }
        uint32_t steps_taken() const { return lanes[0].steps_taken(); }
        // Whether the cell is a hit, before probability is rolled.
        virtual bool hit(uint8_t lane, uint8_t step) const = 0;
        // The cell as stored: a velocity for the MIDI variant, 1 or 0 for gates.
        virtual uint8_t cell(uint8_t lane, uint8_t step) const = 0;

    protected:
        // A lane's step fired.
        virtual void fire(BusManager& bus, uint8_t lane, uint32_t now_us) = 0;
        // Every pass, after the edge logic: outputs that outlast the edge.
        virtual void run(BusManager& bus, uint32_t now_us) = 0;

        StepEngine lanes[LANES];
        Xorshift32 rng;
        uint8_t chance[LANES];

    private:
        EdgeIn advance_in;
        EdgeIn reset_in;
};

// To gates: one gate outlet per lane, each a trigger of fixed width.
//
// Lane data, LANE_STRIDE bytes from LANE_BASE + lane * LANE_STRIDE:
//   [0..3] pattern bits, step 0 in the low bit of [0]
//   [4]    lane length (0 -> the header's)
//   [5]    probability, percent (0 -> always)
//   [6..7] reserved
class DrumSeqGate : public DrumSequencer{
    public:
        static constexpr uint16_t PARAM_COUNT = LANE_BASE + LANES * LANE_STRIDE;

        static const AlgorithmDescriptor descriptor;
        explicit DrumSeqGate(const NodeConfig& config);

        bool hit(uint8_t lane, uint8_t step) const override {
            return lane < LANES && step < MAX_SEQUENCE_LEN && (bits[lane] & ((uint32_t)1u << step)) != 0;
        }
        uint8_t cell(uint8_t lane, uint8_t step) const override { return hit(lane, step) ? 1 : 0; }
        uint32_t pattern(uint8_t lane) const { return lane < LANES ? bits[lane] : 0; }

    protected:
        void fire(BusManager& bus, uint8_t lane, uint32_t now_us) override;
        void run(BusManager& bus, uint32_t now_us) override;

    private:
        uint8_t out[LANES];
        uint32_t bits[LANES];
        TriggerPulse pulse[LANES];
};

// To MIDI: one note outlet; a note number and channel per lane, a velocity
// per cell (0 is no hit).
//
// Lane data, LANE_STRIDE bytes from LANE_BASE + lane * LANE_STRIDE:
//   [0]    note number (0 -> a General MIDI default per lane, see .cpp)
//   [1]    channel 1..16 (0 -> 10)
//   [2]    lane length (0 -> the header's)
//   [3]    probability, percent (0 -> always)
//   [4..7] reserved
// Velocities, from VELOCITY_BASE: [lane * MAX_SEQUENCE_LEN + step].
//
// Release policy: every note-on is released after `gate` milliseconds of
// wall-clock time, from the ledger, so the pitch is the one that was sent
// whatever the lane's note number has become. A lane retriggered before its
// note has been released releases it first; a pattern or lane-length change
// cannot strand a note because the release never looks at the pattern; and
// silence() releases everything when the patch is swapped.
class DrumSeqMidi : public DrumSequencer{
    public:
        static constexpr uint16_t VELOCITY_BASE = LANE_BASE + LANES * LANE_STRIDE;
        static constexpr uint16_t PARAM_COUNT = VELOCITY_BASE + LANES * MAX_SEQUENCE_LEN;
        static constexpr uint8_t DEFAULT_CHANNEL = 10, DEFAULT_GATE_MS = 10;

        static const AlgorithmDescriptor descriptor;
        explicit DrumSeqMidi(const NodeConfig& config);

        void silence(BusManager& bus) override;

        bool hit(uint8_t lane, uint8_t step) const override { return cell(lane, step) != 0; }
        uint8_t cell(uint8_t lane, uint8_t step) const override {
            return (lane < LANES && step < MAX_SEQUENCE_LEN) ? velocity[lane][step] : 0;
        }
        uint8_t lane_note(uint8_t lane) const { return lane < LANES ? note[lane] : 0; }
        uint8_t lane_channel(uint8_t lane) const { return lane < LANES ? channel[lane] : 0; }
        uint8_t sounding_count() const { return sounding.count(); }

    protected:
        void fire(BusManager& bus, uint8_t lane, uint32_t now_us) override;
        void run(BusManager& bus, uint32_t now_us) override;

    private:
        void release(BusManager& bus, uint8_t lane);

        uint8_t out;
        uint32_t gate_us;
        uint8_t note[LANES];
        uint8_t channel[LANES];
        uint8_t velocity[LANES][MAX_SEQUENCE_LEN];
        uint32_t off_at_us[LANES];
        bool playing[LANES];
        SoundingNotes sounding;
};

#endif
