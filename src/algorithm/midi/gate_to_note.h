#ifndef MMMC_ALGORITHM_GATE_TO_NOTE_H
#define MMMC_ALGORITHM_GATE_TO_NOTE_H

#include "node/node.h"

// Gate to MIDI note: a rising edge on the gate inlet sends note on, a
// falling edge sends note off.
//
// The note-off is sent for the note that was sent, not for the note that is
// configured now: change `note` or `channel` while the gate is high (#20) and
// the falling edge still releases what went out, so the parameter cannot
// strand a note on a downstream synth (#10's ownership rule at parameter
// scope). The new note sounds on the next rising edge.
//
// params[0] note (0 -> 60)   params[1] velocity (0 -> 100)   params[2] channel (0 -> 1)
class GateToNote : public Node{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit GateToNote(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

    private:
        uint8_t in;
        uint8_t out;
        uint8_t note;
        uint8_t velocity;
        uint8_t channel;
        uint8_t sent_note;      // what the note-on carried; only valid while `last`
        uint8_t sent_channel;
        bool last;
};

#endif
