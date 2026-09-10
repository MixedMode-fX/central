#ifndef MMMC_CONTROL_CC_MAPPER_H
#define MMMC_CONTROL_CC_MAPPER_H

#include <stdint.h>
#include "config.h"
#include "master.h"
#include "patch/patch_manager.h"

// Bind a controller knob to any parameter (#21).
//
// SysEx (#11) is the right answer for an editor and the wrong answer for a
// performance. A CC is what a musician already has under their fingers, and
// with no encoder, no switches and no display on the module it is the only
// way to change anything while playing.
//
// **A mapping is not a node.** A CC arriving at a MidiInPort already reaches
// a note bus - MidiEvent carries any channel-voice message by design - so it
// is tempting to make mapping an algorithm reading a note bus. Don't. A
// parameter is not a bus signal: it has no domain, no fan-in rule and no
// per-pass value, and writing one is a control-plane operation on the patch,
// which is exactly what #11's incremental edit is. Routing it through the
// graph would mean a mapping only works when the CC's port happens to be
// patched to a bus, would make the mapping unreachable the moment a swap
// removed the node, and would need a pool slot per binding.
//
// So: the table is part of Patch, it applies at the MIDI input layer before
// the graph runs, and it writes through PatchManager::set_param - the same
// validated entry point #11's SysEx edits and the console use. One apply
// path, one validator, one set of tests.
//
// **Rate limiting is at the pass boundary, not per event.** A stuck
// controller or a MIDI loop can hammer a CC thousands of times a second.
// observe() only remembers the newest value per mapping; apply() writes at
// most one value per mapping per pass, whatever arrived in between. That is
// the same "collapse the subticks" discipline MasterClock::consume() uses,
// and with #20's no-op-set rule an unchanged value costs nothing beyond the
// compare.
//
// **A mapping cannot lock the module out.** It cannot target the control
// port, it cannot disable the protocol, and the SysEx handler and console run
// whatever the table says.
class CcMapper {
    public:
        // An armed learn that sees no CC gives up after this, so a module
        // cannot sit armed for ever waiting for a controller nobody touched.
        static constexpr uint32_t LEARN_TIMEOUT_US = 20000000u;

        CcMapper(PatchManager& patches, MixedModeMaster& master);
        CcMapper(const CcMapper&) = delete;
        CcMapper& operator=(const CcMapper&) = delete;

        // Offer one incoming channel-voice message to the table, before it
        // reaches the graph. Returns true if the event was **consumed** -
        // mapped, and not flagged pass-through - in which case it must not be
        // delivered to any note bus.
        //
        // Nothing is written here: a matching CC is remembered and applied at
        // the next pass boundary.
        bool observe(uint8_t source, uint8_t channel, uint8_t cc, uint8_t value, uint32_t now_us);

        // Once per pass, before process(): applies at most one value per
        // mapping. Parameter writes never happen in interrupt context - the
        // transport enqueues, the pass applies - which is the same discipline
        // MasterClock::take_interval_change() already follows.
        void apply(uint32_t now_us);

        // The patch changed under us: forget every knob position, because the
        // targets they were tracking may not exist any more.
        void reset();

        // Learn (#21). With no button to hold, learn is armed over the
        // control channel: the editor or the console says "the next CC you
        // see binds to node 4 parameter 1", and the module answers with what
        // it bound.
        void learn_arm(uint8_t slot, uint8_t target_kind, uint8_t target_index,
                       uint16_t param, uint32_t now_us);
        void learn_cancel();
        bool learning() const { return learn_armed; }
        // True once after a learn captured a CC; clears the flag, so the
        // caller reports the binding to the host exactly once.
        bool take_learn_result(uint8_t& slot_out);

        // Diagnostics.
        uint32_t writes() const { return write_count; }
        uint32_t refused() const { return refuse_count; }

        // What a target's full range is, in the target's own units. Taken
        // from #20's descriptors rather than hardcoded per target, so a
        // mapping's min/max mean the same thing everywhere.
        bool target_range(const CcMapping& m, uint16_t& lo, uint16_t& hi) const;
        bool target_range(uint8_t kind, uint8_t index, uint16_t param,
                          uint16_t& lo, uint16_t& hi) const;

        // Write one target directly. NRPN (#22) is a second *transport* for
        // parameter writes, not a second semantics, so it ends here - at the
        // same applier a mapped CC uses, which ends at
        // PatchManager::set_param.
        // `transient` marks a write that will be made again next pass - the
        // modulation matrix's. Such a write reaches the node and nothing
        // else: it is not mirrored into the patch image and does not mark the
        // store dirty, because a modulator running for an hour must not be an
        // hour of EEPROM writes or a preset that saves whatever phase its LFO
        // was at (PatchManager::modulate_param says this at length).
        bool write_control(uint8_t kind, uint8_t index, uint16_t param,
                           uint16_t value, uint32_t now_us, bool transient = false);
        // What a target currently holds, for a host that wants to read one
        // without a full dump.
        bool read_control(uint8_t kind, uint8_t index, uint16_t param, uint16_t& value_out) const;

    private:
        // Per-mapping state that is *not* part of the patch: where the knob
        // physically is, and whether it has caught up with its target.
        struct Knob {
            uint16_t pending;      // the newest value seen since the last pass
            uint16_t last_lsb;     // for a 14-bit pair
            uint16_t last_applied; // what was last written, for pickup / scale
            bool has_pending;
            bool have_lsb;
            bool engaged;          // pickup / scale have caught up
            bool last_switch;      // a transport target's last level
            // Scale mode's anchor, frozen when the knob is first moved: the
            // position it was at, and the value the target was at. Everything
            // afterwards is measured from these, so the mapping stays
            // continuous and reversible instead of compounding.
            uint16_t anchor_pos;
            uint16_t anchor_value;
            uint8_t physical;      // the last raw 7-bit position seen
            bool have_physical;
        };

        void apply_one(uint8_t index, uint32_t now_us);
        void write_target(const CcMapping& m, uint16_t value, uint32_t now_us);
        // Maps a 0..full controller position onto [lo, hi].
        static uint16_t scale_in(uint32_t position, uint32_t full, uint16_t lo, uint16_t hi);
        // Decodes one of the three relative encodings; 0 when the mapping is
        // absolute or the value means "no movement".
        static int8_t relative_delta(uint8_t flags, uint8_t value);

        PatchManager& patches;
        MixedModeMaster& mm;
        Knob knobs[N_CC_MAP];

        uint32_t write_count;
        uint32_t refuse_count;

        // Learn
        CcMapping learn_template;
        uint32_t learn_armed_us;
        uint8_t learn_slot;
        uint8_t learn_result_slot;
        bool learn_armed;
        bool learn_result;
};

#endif
