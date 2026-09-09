#ifndef MMMC_CONTROL_NRPN_H
#define MMMC_CONTROL_NRPN_H

#include <stdint.h>
#include "config.h"
#include "control/cc_mapper.h"

// The addressed parameter channel (#22).
//
// CC (#21) binds a knob to one parameter and is what a performance wants. It
// cannot be the whole story: CC has 120 usable numbers and a 7-bit value, and
// this module has N_NODE x N_PARAM = 10752 parameters before the clock and
// the transport are counted. NRPN is how synths have solved exactly this
// since the 1990s - fourteen bits of address and fourteen of value - and it
// is a second *transport* for parameter writes, not a second semantics: it
// reaches the same target space #21 defines and ends at the same
// PatchManager::set_param.
//
// The rule that decides the tier is address space and payload width, not
// importance: **if it changes the graph's shape it is SysEx; if it changes a
// value inside a node it is CC or NRPN.**
//
// ## The address space, written down and versioned
//
// Fourteen bits, laid out so the common case is dense and nothing needs a
// second lookup table:
//
//   0x0000 .. 0x29FF   a node's parameter.
//                      node  = address / N_PARAM
//                      param = address % N_PARAM
//                      (N_NODE * N_PARAM = 32 * 336 = 10752 = 0x2A00)
//   0x2A00 .. 0x2A0F   the master clock: address - 0x2A00 is a CcClockTarget.
//   0x2A10 .. 0x2A1F   the transport: address - 0x2A10 is a CcTransportTarget.
//   0x2A20 .. 0x3FFF   reserved.
//
// It is reported in the capability message, so an editor reads the layout
// rather than hardcoding it, and the protocol version moves if it changes.
#define NRPN_CLOCK_BASE     0x2A00u
#define NRPN_TRANSPORT_BASE 0x2A10u
#define NRPN_RESERVED_BASE  0x2A20u

// A four-message NRPN can be interleaved with other traffic, arrive out of
// order, or stop halfway. A sequence older than this is abandoned rather than
// pairing an address from one gesture with a value from the next.
#define NRPN_TIMEOUT_US 2000000u

// Decoder for one incoming CC stream (#22).
//
// **Off by default, enabled per port and channel.** CC 99, 98, 6 and 38 look
// like ordinary CCs to everything upstream, so a module that always consumed
// them would silently eat NRPN-shaped traffic on its way to a downstream
// synth. Enabling it is a deliberate act, recorded in the patch.
//
// **A partial sequence writes nothing.** The address is buffered per channel
// and applied on the data MSB; a sequence that never gets there times out and
// leaves everything alone.
class NrpnDecoder {
    public:
        NrpnDecoder(PatchManager& patches, CcMapper& mapper);
        NrpnDecoder(const NrpnDecoder&) = delete;
        NrpnDecoder& operator=(const NrpnDecoder&) = delete;

        // Offer one incoming CC. Returns true if it was consumed as part of an
        // NRPN sequence, in which case it must not reach a note bus. Returns
        // false - so the CC passes through untouched - when NRPN is not
        // enabled for this port and channel, or the CC is not one of the six
        // NRPN controllers.
        bool observe(uint8_t source, uint8_t channel, uint8_t cc, uint8_t value, uint32_t now_us);

        // Once per main loop: abandons a stale partial sequence.
        void service(uint32_t now_us);

        // Resolves an address to a target. False for a reserved or
        // out-of-range address, which is why a bad address writes nothing
        // rather than writing somewhere arbitrary.
        static bool resolve(uint16_t address, uint8_t& kind, uint8_t& index, uint16_t& param);
        // The inverse, for the console and the editor.
        static bool address_of(uint8_t kind, uint8_t index, uint16_t param, uint16_t& address);

        // Diagnostics.
        uint32_t writes() const { return write_count; }
        uint32_t refused() const { return refuse_count; }
        bool in_sequence() const { return have_address; }
        uint16_t last_address() const { return address; }

    private:
        bool enabled_for(uint8_t source, uint8_t channel) const;
        void write(uint16_t value, uint32_t now_us);
        bool current_value(uint16_t& value_out) const;

        PatchManager& patches;
        CcMapper& cc;

        uint16_t address;
        uint8_t data_msb;
        uint32_t last_message_us;
        uint8_t sequence_channel;
        bool have_address;
        bool have_data_msb;

        uint32_t write_count;
        uint32_t refuse_count;
};

#endif
