#ifndef MMMC_PROTOCOL_SYSEX_HANDLER_H
#define MMMC_PROTOCOL_SYSEX_HANDLER_H

#include <stdint.h>
#include "config.h"
#include "hal/isysex_in.h"
#include "hal/imidi_out.h"
#include "protocol/sysex.h"
#include "patch/patch_manager.h"
#include "control/cc_mapper.h"
#include "led/status_leds.h"

// The patch protocol's device end (#11).
//
// **It is not part of the graph.** It is not a node, it holds no bus index,
// and nothing a patch can express reaches it. That is deliberate and it is
// the property the whole feature rests on: with no button to hold at
// power-on, a malformed patch that could take the protocol down with it would
// leave reflashing over USB as the only way to recover. So a patch cannot
// disable the handler, cannot reroute the port it answers on, and cannot
// starve it - and the test for that is to send garbage, then a good patch,
// and watch the good one land.
//
// **Everything goes through PatchManager.** Bulk loads, incremental edits,
// preset recall and Program Change all end at the same staging buffer, the
// same validator and the same swap, which is what makes "the editor, the
// console and an eventual Launchpad write the same bytes" true rather than
// aspirational.
//
// **The control cable.** The build is USB_MIDI4_SERIAL and cable 3
// (MIDI_CONTROL_PORT) is reserved for control traffic, so a patch transfer
// never mixes with musical MIDI and a busy note stream cannot starve a dump.
// Incoming SysEx is accepted from any port - a DIN librarian has to work -
// but nothing musical is ever routed to cable 3, and a reply always goes back
// to the port the request came in on.
class SysexHandler : public ISysexIn {
    public:
        // When a patch swap is quantised, where the boundary is.
        //
        // A Program Change mid-bar that swaps instantly glitches; one that
        // waits for a musical boundary does not. Bars are counted as four
        // beats: the module has no time signature, and four is the assumption
        // every drum machine makes.
        enum SwapTiming : uint8_t {
            SWAP_IMMEDIATE = 0,
            SWAP_NEXT_BEAT = 1,
            SWAP_NEXT_BAR  = 2,
        };
        static constexpr uint8_t BEATS_PER_BAR = 4;

        SysexHandler(PatchManager& patches, MixedModeMaster& master,
                     PatchStore& store, StatusLeds& leds, IMidiOut& midi, CcMapper& mapper);
        SysexHandler(const SysexHandler&) = delete;
        SysexHandler& operator=(const SysexHandler&) = delete;

        // A complete incoming message, F0 to F7 inclusive.
        void deliver_sysex(uint8_t source, const uint8_t* data, uint16_t length) override;

        // Once per main loop: abandons a stalled transfer and applies a
        // pending quantised swap when its boundary arrives.
        void service(uint32_t now_us);

        // A Program Change seen on a note bus's way in (#11). Returns true if
        // it was consumed as a preset recall - which happens only when recall
        // is enabled and the channel and port match, so a Program Change
        // meant for a downstream synth is not silently swallowed.
        bool program_change(uint8_t source, uint8_t channel, uint8_t program, uint32_t now_us);

        // This module's id, so two on one bus can be addressed separately.
        void set_device_id(uint8_t id){ device = (uint8_t)(id & 0x7F); }
        uint8_t device_id() const { return device; }

        void set_swap_timing(uint8_t how){ timing = how; }
        uint8_t swap_timing() const { return timing; }

        // Diagnostics, for the console and the red LED.
        bool transfer_in_progress() const { return receiving; }
        bool swap_pending() const { return pending; }
        uint32_t rejected() const { return reject_count; }
        SysexError last_error() const { return error; }

    private:
        void handle_universal(uint8_t source, const uint8_t* data, uint16_t length, uint32_t now_us);
        void handle_command(uint8_t source, uint8_t command, const uint8_t* args, uint16_t n, uint32_t now_us);

        void reply_identity(uint8_t source);
        void reply_universal_identity(uint8_t source);
        void reply_capabilities(uint8_t source);
        void reply_algorithms(uint8_t source);
        void reply_param_descriptors(uint8_t source, uint8_t algorithm_id);
        void reply_dump(uint8_t source);
        void reply_slots(uint8_t source);
        void reply_cc_map(uint8_t source, uint8_t slot);
        void reply_pattern(uint8_t source, uint8_t node, uint16_t offset, uint16_t length);
        void ack(uint8_t source);
        void nak(uint8_t source, SysexError code);
        void notify(uint8_t event, uint8_t detail);

        void begin_reply(uint8_t command);
        void put(uint8_t value){ if (tx_at < SYSEX_TX_MAX - 1u) tx[tx_at++] = (uint8_t)(value & 0x7F); }
        // A length-prefixed ASCII string. The default cap suits a name; an
        // algorithm's one-line summary asks for SUMMARY_MAX, and both are
        // budgeted against SYSEX_TX_MAX by test_params.
        static constexpr uint8_t TEXT_MAX = 24;
        static constexpr uint8_t SUMMARY_MAX = 96;
        void put_string(const char* text, uint8_t limit = TEXT_MAX);
        void put_u14(uint16_t value){ put((uint8_t)(value & 0x7F)); put((uint8_t)((value >> 7) & 0x7F)); }
        void send_reply(uint8_t source);

        void abort_transfer();
        void receive_chunk(uint8_t source, const uint8_t* args, uint16_t n, uint32_t now_us);
        void arm_swap(uint32_t now_us);
        // The subtick count the pending swap is waiting for, or 0 when the
        // timing is immediate.
        uint32_t swap_boundary() const;

        PatchManager& patches;
        MixedModeMaster& mm;
        PatchStore& store;
        StatusLeds& leds;
        IMidiOut& midi;
        CcMapper& cc;

        // The staging image a bulk transfer accumulates into. The live graph
        // is untouched until the last chunk has arrived and the whole image
        // has validated.
        uint8_t staging[PATCH_SLOT_BYTES];
        uint16_t staged;
        uint8_t next_seq;
        uint32_t transfer_started_us;
        uint8_t transfer_source;
        bool receiving;

        // A swap waiting for a musical boundary.
        Patch pending_patch;
        GlobalSettings pending_globals;
        uint32_t pending_at;
        uint8_t pending_slot;         // 0xFF when the swap did not come from a slot
        bool pending;

        uint8_t tx[SYSEX_TX_MAX];
        uint16_t tx_at;
        uint8_t reply_to;             // the port the last notification goes to

        uint32_t reject_count;
        SysexError error;
        uint8_t device;
        uint8_t timing;
};

#endif
