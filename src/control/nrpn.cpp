#include "control/nrpn.h"
#include "hal/midi_types.h"

// The six controllers an NRPN uses.
static constexpr uint8_t CC_DATA_MSB     = 6;
static constexpr uint8_t CC_DATA_LSB     = 38;
static constexpr uint8_t CC_DATA_INC     = 96;
static constexpr uint8_t CC_DATA_DEC     = 97;
static constexpr uint8_t CC_NRPN_LSB     = 98;
static constexpr uint8_t CC_NRPN_MSB     = 99;

static_assert((uint32_t)N_NODE * N_PARAM == NRPN_CLOCK_BASE,
              "the node address block must end exactly where the clock block starts");
static_assert(NRPN_RESERVED_BASE <= 0x4000u, "the address space is fourteen bits");

NrpnDecoder::NrpnDecoder(PatchManager& manager, CcMapper& mapper) :
    patches(manager), cc(mapper),
    address(0), data_msb(0), last_message_us(0), sequence_channel(0),
    have_address(false), have_data_msb(false),
    write_count(0), refuse_count(0)
{}

bool NrpnDecoder::resolve(uint16_t addr, uint8_t& kind, uint8_t& index, uint16_t& param){
    if (addr < NRPN_CLOCK_BASE){
        kind = CC_TARGET_NODE;
        index = (uint8_t)(addr / N_PARAM);
        param = (uint16_t)(addr % N_PARAM);
        return true;
    }
    if (addr < NRPN_TRANSPORT_BASE){
        kind = CC_TARGET_CLOCK;
        index = 0;
        param = (uint16_t)(addr - NRPN_CLOCK_BASE);
        return param < CC_CLOCK_TARGETS;
    }
    if (addr < NRPN_KEY_BASE){
        kind = CC_TARGET_TRANSPORT;
        index = 0;
        param = (uint16_t)(addr - NRPN_TRANSPORT_BASE);
        return param < CC_TRANSPORT_TARGETS;
    }
    if (addr < NRPN_RESERVED_BASE){
        kind = CC_TARGET_KEY;
        index = 0;
        param = (uint16_t)(addr - NRPN_KEY_BASE);
        return param < CC_KEY_TARGETS;
    }
    return false;
}

bool NrpnDecoder::address_of(uint8_t kind, uint8_t index, uint16_t param, uint16_t& addr){
    switch (kind){
        case CC_TARGET_NODE:
            if (index >= N_NODE || param >= N_PARAM) return false;
            addr = (uint16_t)((uint32_t)index * N_PARAM + param);
            return true;
        case CC_TARGET_CLOCK:
            if (param >= CC_CLOCK_TARGETS) return false;
            addr = (uint16_t)(NRPN_CLOCK_BASE + param);
            return true;
        case CC_TARGET_TRANSPORT:
            if (param >= CC_TRANSPORT_TARGETS) return false;
            addr = (uint16_t)(NRPN_TRANSPORT_BASE + param);
            return true;
        case CC_TARGET_KEY:
            if (param >= CC_KEY_TARGETS) return false;
            addr = (uint16_t)(NRPN_KEY_BASE + param);
            return true;
        default:
            return false;
    }
}

// Off by default, and enabled per port and channel: NRPN looks like ordinary
// CC traffic to everything upstream, so a module that always consumed it
// would silently eat a stream on its way to a downstream synth.
bool NrpnDecoder::enabled_for(uint8_t source, uint8_t channel) const {
    const GlobalSettings& g = patches.globals();
    if (!g.nrpn_enabled) return false;
    if (g.nrpn_source_mask != 0 && (g.nrpn_source_mask & source) == 0) return false;
    if (g.nrpn_channel != 0 && g.nrpn_channel != channel) return false;
    return true;
}

bool NrpnDecoder::current_value(uint16_t& value_out) const {
    uint8_t kind = 0, index = 0;
    uint16_t param = 0;
    if (!resolve(address, kind, index, param)) return false;
    return cc.read_control(kind, index, param, value_out);
}

void NrpnDecoder::write(uint16_t value, uint32_t now_us){
    uint8_t kind = 0, index = 0;
    uint16_t param = 0;
    if (!resolve(address, kind, index, param)){ refuse_count++; return; }

    // Clamp into the target's own range rather than writing something the
    // validator would refuse: an NRPN carries fourteen bits and most targets
    // take eight, so the top of the data range would otherwise always be a
    // rejection.
    uint16_t lo = 0, hi = 0;
    if (!cc.target_range(kind, index, param, lo, hi)){ refuse_count++; return; }
    if (value < lo) value = lo;
    if (value > hi) value = hi;

    if (cc.write_control(kind, index, param, value, now_us)) write_count++;
    else refuse_count++;
}

bool NrpnDecoder::observe(uint8_t source, uint8_t channel, uint8_t controller,
                          uint8_t value, uint32_t now_us){
    if (!enabled_for(source, channel)) return false;

    switch (controller){
        case CC_NRPN_MSB:
            // A new address always starts a new sequence: an MSB arriving
            // mid-gesture means the host moved on.
            address = (uint16_t)((address & 0x007Fu) | ((uint16_t)(value & 0x7F) << 7));
            sequence_channel = channel;
            have_address = true;
            have_data_msb = false;
            last_message_us = now_us;
            return true;

        case CC_NRPN_LSB:
            address = (uint16_t)((address & 0x3F80u) | (value & 0x7F));
            sequence_channel = channel;
            have_address = true;
            have_data_msb = false;
            last_message_us = now_us;
            return true;

        case CC_DATA_MSB:
            // The write happens here, on the data MSB, which is the standard
            // behaviour. A data byte with no address before it writes
            // nothing at all.
            if (!have_address || channel != sequence_channel){ refuse_count++; return true; }
            data_msb = (uint8_t)(value & 0x7F);
            have_data_msb = true;
            last_message_us = now_us;
            write((uint16_t)((uint16_t)data_msb << 7), now_us);
            return true;

        case CC_DATA_LSB:
            // The LSB refines the value the MSB already applied, so a
            // controller that sends both lands on the exact value one message
            // later. On its own it writes nothing.
            if (!have_address || !have_data_msb || channel != sequence_channel){
                refuse_count++;
                return true;
            }
            last_message_us = now_us;
            write((uint16_t)(((uint16_t)data_msb << 7) | (value & 0x7F)), now_us);
            return true;

        case CC_DATA_INC:
        case CC_DATA_DEC: {
            // How a controller nudges a parameter without knowing its current
            // value - which is exactly why the current value is read here
            // rather than tracked.
            if (!have_address || channel != sequence_channel){ refuse_count++; return true; }
            last_message_us = now_us;
            uint16_t current = 0;
            if (!current_value(current)){ refuse_count++; return true; }
            const uint16_t step = (uint16_t)(value ? value : 1u);
            const int32_t next = (controller == CC_DATA_INC)
                               ? (int32_t)current + step
                               : (int32_t)current - step;
            write((uint16_t)(next < 0 ? 0 : next), now_us);
            return true;
        }

        default:
            // Not an NRPN controller: it belongs to the graph, or to a
            // mapping, and this decoder has no opinion about it.
            return false;
    }
}

void NrpnDecoder::service(uint32_t now_us){
    // A sequence that stopped halfway must not pair an address from one
    // gesture with a value from the next.
    if (have_address && (uint32_t)(now_us - last_message_us) > NRPN_TIMEOUT_US){
        have_address = false;
        have_data_msb = false;
    }
}
