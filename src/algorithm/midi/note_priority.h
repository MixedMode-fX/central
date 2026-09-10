#ifndef MMMC_ALGORITHM_NOTE_PRIORITY_H
#define MMMC_ALGORITHM_NOTE_PRIORITY_H

#include "node/node.h"
#include "midi/held_notes.h"
#include "midi/sounding_notes.h"

// Monophonic note priority: many notes in, one note out.
//
// Which one wins is the parameter, and all three answers are wanted by
// somebody: `low` is a bass line, `high` is a lead, `latest` is what a
// keyboard player expects. Releasing a note hands the voice back to whichever
// of the remaining held notes wins - which is why arrival order has to be
// kept rather than inferred.
//
// params[0] mode  0 = lowest, 1 = highest, 2 = latest
class NotePriority : public Node{
    public:
        // The stored parameter, which is the preset format and cannot be
        // renumbered - and which happens to be NotePriorityRule's own
        // numbering, because this node's default is the first of them.
        enum Mode : uint8_t {
            PRIORITY_LOW    = NOTE_PRIORITY_LOWEST,
            PRIORITY_HIGH   = NOTE_PRIORITY_HIGHEST,
            PRIORITY_LATEST = NOTE_PRIORITY_LATEST,
        };

        static const AlgorithmDescriptor descriptor;
        explicit NotePriority(const NodeConfig& config);
        void process(BusManager& bus, uint32_t) override;
        void silence(BusManager& bus) override;
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        uint8_t held_count() const { return held.count(); }

    private:
        void follow(BusManager& bus);

        uint8_t in;
        uint8_t out;
        uint8_t mode;
        uint8_t playing;          // HeldNotes::NONE when silent
        HeldNotes held;
        SoundingNotes sounding;
};

#endif
