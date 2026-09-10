#ifndef MMMC_ALGORITHM_UTIL_GATE_HOLD_H
#define MMMC_ALGORITHM_UTIL_GATE_HOLD_H

#include "node/node.h"

// Hold a gate high.
//
// A trigger is five milliseconds wide (config.h). Everything in the module
// that makes one - a divider, a metronome, a sequencer - makes that, because
// a trigger whose width followed the tempo would stop being a trigger. That
// is right for clocking things and useless for the other half of what a gate
// is for: opening an envelope, latching a mute, holding a note down. There
// was no way to turn one into the other, so a jack could be triggered and
// never *held*.
//
// This is the missing piece, in four modes. The first two hold indefinitely
// and are cleared by an event; the second two hold for a time and are cleared
// by the clock.
//
//   latch   a rising edge on `set` raises the output and it stays up. A
//           rising edge on `reset` drops it. An SR latch: the state survives
//           any number of passes, so this is the mode that answers "hold a
//           gate high until I say otherwise". With nothing patched to
//           `reset`, it stays up until the patch changes - which is a
//           legitimate thing to ask for and is why `reset` is optional.
//   toggle  each rising edge on `set` flips the output; `reset` still drops
//           it. One button, two states - a mute, a hold, a fill.
//   extend  a minimum length. The output follows `set`, but once raised it
//           stays up for at least `hold`, retriggering cleanly if another
//           edge arrives while it is still up. This is what turns the
//           module's 5 ms triggers into gates.
//   limit   a maximum length. The output follows `set` but drops after
//           `hold` even if the input is still up, and does not rise again
//           until the input has gone low and come back. A long gate from a
//           sustain pedal or a held key, cut to a fixed length.
//
// **Reset wins.** A reset edge in the same pass as a set edge leaves the
// output low, in every mode. One rule, stated once, rather than four
// orderings a user has to discover.
//
// Inlet 0 (gate, required): set.
// Inlet 1 (gate, optional): reset.
// Outlet 0 (gate): the held level. Written every pass it is up, because a
//         gate bus is cleared by the swap - the level is the node's state,
//         not the bus's.
//
// params[0] mode  latch / toggle / extend / limit
// params[1] hold  milliseconds, for extend and limit (0 -> DEFAULT_HOLD_MS)
//
// Live edits (#20): both move at runtime. Changing the mode does not clear
// the output - a latch turned into a toggle is still up, and the next edge
// flips it - because a mode change is not a reset and silently dropping a
// gate somebody is holding is worse than either reading.
class GateHold : public Node{
    public:
        static const AlgorithmDescriptor descriptor;

        // Indexed from 1, so a stored 0 still means the descriptor's default
        // (node/param.h).
        enum Mode : uint8_t {
            HOLD_LATCH  = 1,
            HOLD_TOGGLE = 2,
            HOLD_EXTEND = 3,
            HOLD_LIMIT  = 4,
            HOLD_MODES  = 4,
        };

        // Long enough to be unmistakably a gate rather than a trigger, and
        // short enough that a user who has not set it yet hears the note stop.
        static constexpr uint8_t DEFAULT_HOLD_MS = 100;

        explicit GateHold(const NodeConfig& config);

        void process(BusManager& bus, uint32_t now_us) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // Diagnostics / tests
        bool held() const { return level; }
        uint8_t mode() const { return how; }
        uint32_t hold_us() const {
            return (uint32_t)(hold_param ? hold_param : DEFAULT_HOLD_MS) * 1000u;
        }

    private:
        uint8_t set_in;
        uint8_t reset_in;
        uint8_t out;
        uint8_t how;             // Mode
        uint8_t hold_param;      // milliseconds, as stored
        uint32_t since_us;       // when the timed modes started counting
        bool timing;
        bool level;              // what the outlet is driven to
        bool last_set;
        bool last_reset;
};

#endif
