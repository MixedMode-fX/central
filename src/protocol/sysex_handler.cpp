#include "protocol/sysex_handler.h"
#include "patch/default_patch.h"
#include "node/registry.h"
#include "control/nrpn.h"
#include "version.h"

// Message layout, after the F0 and before the F7:
//
//   0  SYSEX_MANUFACTURER  (0x7D, development only)
//   1  device id           this module's id, or 0x7F to address any
//   2  command             SysexCommand
//   3  protocol version    SYSEX_PROTOCOL_VERSION
//   4+ arguments           command-specific, every byte <= 0x7F
//
// The version sits inside every message rather than only in a handshake, so
// an older editor talking to newer firmware is refused per message and
// cannot get half a transfer in before anyone notices.

static constexpr uint16_t HEADER_BYTES = 4;

SysexHandler::SysexHandler(PatchManager& manager, MixedModeMaster& master,
                           PatchStore& patch_store, StatusLeds& status, IMidiOut& midi_out,
                           CcMapper& mapper) :
    patches(manager), mm(master), store(patch_store), leds(status), midi(midi_out), cc(mapper),
    staging(), staged(0), next_seq(0), transfer_started_us(0), transfer_source(0),
    receiving(false),
    pending_patch(empty_patch()), pending_globals(default_globals()),
    pending_at(0), pending_slot(0xFF), pending(false),
    tx(), tx_at(0), reply_to(MIDI_CONTROL_PORT),
    reject_count(0), error(SYSEX_ERR_NONE),
    device(SYSEX_DEFAULT_DEVICE), timing(SWAP_IMMEDIATE)
{}

// Replies ---------------------------------------------------------------

void SysexHandler::begin_reply(uint8_t command){
    tx_at = 0;
    tx[tx_at++] = 0xF0;
    tx[tx_at++] = SYSEX_MANUFACTURER;
    tx[tx_at++] = device;
    tx[tx_at++] = command;
    tx[tx_at++] = SYSEX_PROTOCOL_VERSION;
}

void SysexHandler::put_string(const char* text){
    // Length-prefixed and ASCII-clamped: a name is written by us, but the
    // length has to be on the wire for an editor to parse a variable-length
    // record without guessing.
    uint8_t n = 0;
    while (text != nullptr && text[n] != '\0' && n < 24) n++;
    put(n);
    for (uint8_t i = 0; i < n; i++){
        const char c = text[i];
        put((uint8_t)((c >= 32 && c <= 126) ? c : '?'));
    }
}

void SysexHandler::send_reply(uint8_t source){
    if (tx_at + 1u > SYSEX_TX_MAX) return;
    tx[tx_at++] = 0xF7;
    midi.send_sysex(source, tx, tx_at);
}

void SysexHandler::ack(uint8_t source){
    begin_reply(SYSEX_ACK);
    send_reply(source);
}

void SysexHandler::nak(uint8_t source, SysexError code){
    error = code;
    reject_count++;
    begin_reply(SYSEX_NAK);
    put(code);
    send_reply(source);
}

// The module speaking first: only for state it originates itself. With no
// panel there is no user-initiated drift for an editor to reconcile.
void SysexHandler::notify(uint8_t event, uint8_t detail){
    begin_reply(SYSEX_EVENT);
    put(event);
    put(detail);
    send_reply(reply_to);
}

// Receiving -------------------------------------------------------------

void SysexHandler::deliver_sysex(uint8_t source, const uint8_t* data, uint16_t length){
    if (data == nullptr || length < 4) return;
    if (data[0] != 0xF0) return;

    // A message longer than the buffer is dropped rather than truncated: a
    // truncated command is a command with the wrong arguments.
    if (length > SYSEX_RX_MAX){
        nak(source, SYSEX_ERR_TOO_LARGE);
        return;
    }
    // The trailing F7 may or may not be included by the transport.
    uint16_t end = length;
    if (data[end - 1u] == 0xF7) end--;

    if (data[1] == SYSEX_UNIVERSAL_NON_REALTIME){
        handle_universal(source, data, end, 0);
        return;
    }
    if (data[1] != SYSEX_MANUFACTURER) return;      // somebody else's device

    if (end < 1u + HEADER_BYTES) return;
    const uint8_t addressed = data[2];
    if (addressed != device && addressed != SYSEX_BROADCAST_DEVICE) return;

    const uint8_t command = data[3];
    // Replies are 0x40 and above; a module that hears its own reply looped
    // back through a host must not act on it.
    if (command >= 0x40) return;

    if (data[4] != SYSEX_PROTOCOL_VERSION){
        nak(source, SYSEX_ERR_BAD_VERSION);
        return;
    }
    reply_to = source;
    handle_command(source, command, &data[1u + HEADER_BYTES], (uint16_t)(end - 1u - HEADER_BYTES), 0);
}

// The standard identity request, so an editor finds the module among the
// host's ports instead of making a user pick one by name.
void SysexHandler::handle_universal(uint8_t source, const uint8_t* data, uint16_t length, uint32_t){
    if (length < 5) return;
    if (data[3] != SYSEX_GENERAL_INFORMATION || data[4] != SYSEX_IDENTITY_REQUEST) return;
    const uint8_t addressed = data[2];
    if (addressed != device && addressed != SYSEX_BROADCAST_DEVICE) return;
    reply_to = source;
    reply_universal_identity(source);
    // Both LEDs, so a user with two modules can see which one answered.
    leds.identify(0);
}

void SysexHandler::handle_command(uint8_t source, uint8_t command,
                                  const uint8_t* args, uint16_t n, uint32_t now_us){
    switch (command){
        case SYSEX_HELLO:         reply_identity(source); leds.identify(now_us); return;
        case SYSEX_CAPS_REQUEST:  reply_capabilities(source); return;
        case SYSEX_ALGO_REQUEST:  reply_algorithms(source); return;

        case SYSEX_PARAM_REQUEST:
            if (n < 1){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            reply_param_descriptors(source, args[0]);
            return;

        case SYSEX_DUMP_REQUEST:  reply_dump(source); return;
        case SYSEX_PATCH_CHUNK_IN: receive_chunk(source, args, n, now_us); return;

        case SYSEX_PATCH_ABORT:
            abort_transfer();
            ack(source);
            return;

        case SYSEX_SET_PARAM: {
            if (n < 4){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            const uint16_t param = (uint16_t)(args[1] | ((uint16_t)args[2] << 7));
            switch (patches.set_param(args[0], param, args[3], now_us)){
                case PARAM_SET_OK: ack(source); return;
                case PARAM_VALUE_OUT_OF_RANGE:
                case PARAM_REFUSED: nak(source, SYSEX_ERR_BAD_ARGUMENT); return;
                default: nak(source, SYSEX_ERR_BAD_ARGUMENT); return;
            }
        }

        case SYSEX_GET_PARAM: {
            if (n < 3){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            const uint16_t param = (uint16_t)(args[1] | ((uint16_t)args[2] << 7));
            uint8_t value = 0;
            if (!mm.get_node_param(args[0], param, value)){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            begin_reply(SYSEX_PARAM_VALUE);
            put(args[0]);
            put_u14(param);
            put(value);
            send_reply(source);
            return;
        }

        // One inlet or outlet of one node. Under the bus model this is one
        // byte in the config, with no re-sort, no graph rebuild and no cycle
        // re-check - which is what makes dragging a cable in the editor a
        // single message rather than a full dump.
        case SYSEX_SET_CONNECTION: {
            if (n < 4){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            const uint8_t node = args[0];
            const uint8_t is_out = args[1];
            const uint8_t index = args[2];
            // 0x7F on the wire means NO_BUS: bus indices are small, and 0xFF
            // is not a legal SysEx data byte.
            const uint8_t bus = (args[3] == 0x7F) ? NO_BUS : args[3];
            if (node >= patches.active().n_nodes){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            if ((is_out ? index >= MAX_OUT : index >= MAX_IN)){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }

            patches.begin_edit();
            NodeConfig& c = patches.staging().nodes[node];
            if (is_out) c.out_bus[index] = bus; else c.in_bus[index] = bus;
            if (patches.commit_node(node, now_us) != APPLY_OK){ nak(source, SYSEX_ERR_REJECTED); return; }
            ack(source);
            return;
        }

        case SYSEX_SET_GATE_PORT: {
            if (n < 3){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            GatePortConfig c{args[1], (uint8_t)(args[2] == 0x7F ? NO_BUS : args[2])};
            if (patches.commit_gate_port(args[0], c, now_us) != APPLY_OK){
                nak(source, SYSEX_ERR_REJECTED);
                return;
            }
            ack(source);
            return;
        }

        case SYSEX_SET_MIDI_PORT: {
            if (n < 5){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            // The mask is seven bits on the wire plus one carried in the
            // direction byte's high bit, because a port mask reaches 0x80.
            const uint8_t mask = (uint8_t)(args[2] | ((args[1] & 0x02) ? 0x80u : 0u));
            const bool is_out = (args[1] & 0x01) != 0;
            const ApplyError e = is_out
                ? patches.commit_midi_out(args[0], MidiOutConfig{mask, args[3], (uint8_t)(args[4] == 0x7F ? NO_BUS : args[4])}, now_us)
                : patches.commit_midi_in(args[0], MidiInConfig{mask, args[3], (uint8_t)(args[4] == 0x7F ? NO_BUS : args[4])}, now_us);
            if (e != APPLY_OK){ nak(source, SYSEX_ERR_REJECTED); return; }
            ack(source);
            return;
        }

        case SYSEX_SET_GLOBALS: {
            if (n < 8){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            GlobalSettings g = patches.globals();
            g.clock_source = args[0];
            g.cv_ppqn = args[1];
            g.bpm = (uint16_t)(args[2] | ((uint16_t)args[3] << 7));
            g.pc_enabled = args[4];
            g.pc_channel = args[5];
            g.pc_source_mask = args[6];
            g.pc_quantise = args[7];
            if (g.clock_source > MasterClock::CLOCK_MIDI || g.pc_quantise > SWAP_NEXT_BAR){
                nak(source, SYSEX_ERR_BAD_ARGUMENT);
                return;
            }
            timing = g.pc_quantise;
            patches.set_globals(g, now_us);
            ack(source);
            return;
        }

        // One controller binding (#21). The whole record in one message: a
        // mapping is ten bytes, so chunking it would be ceremony.
        case SYSEX_SET_CC_MAP: {
            if (n < 11){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            if (args[0] >= N_CC_MAP){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            CcMapping m = unused_mapping();
            // The port mask reaches 0x80, so its top bit rides in the flags
            // byte's spare bit rather than being truncated on the wire.
            m.source_mask = (uint8_t)(args[1] | ((args[10] & 0x40) ? 0x80u : 0u));
            m.channel = args[2];
            m.cc = args[3];
            m.target_kind = args[4];
            m.target_index = args[5];
            m.param = (uint16_t)(args[6] | ((uint16_t)args[7] << 7));
            m.min = (uint16_t)(args[8] | ((uint16_t)(args[9] & 0x7F) << 7));
            m.max = (uint16_t)(n > 12 ? (args[11] | ((uint16_t)(args[12] & 0x7F) << 7)) : 0u);
            m.flags = (uint8_t)(args[10] & 0x3F);
            patches.begin_edit();
            patches.staging().cc_map[args[0]] = m;
            if (patches.commit_cc_map(args[0], now_us) != APPLY_OK){
                nak(source, SYSEX_ERR_REJECTED);
                return;
            }
            cc.reset();
            ack(source);
            return;
        }

        case SYSEX_GET_CC_MAP:
            if (n < 1 || args[0] >= N_CC_MAP){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            reply_cc_map(source, args[0]);
            return;

        // Learn, without a panel: the host says "the next CC you see binds to
        // this target", and the module answers with what it bound.
        case SYSEX_CC_LEARN: {
            if (n < 1){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            if (args[0] == 0){ cc.learn_cancel(); ack(source); return; }
            if (n < 5 || args[1] >= N_CC_MAP){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            const uint16_t param = (uint16_t)(args[4] | ((uint16_t)(n > 5 ? args[5] : 0u) << 7));
            cc.learn_arm(args[1], args[2], args[3], param, now_us);
            ack(source);
            return;
        }

        // NRPN, off by default and enabled deliberately (#22).
        case SYSEX_SET_NRPN: {
            if (n < 3){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            GlobalSettings g = patches.globals();
            g.nrpn_enabled = args[0] ? 1u : 0u;
            g.nrpn_channel = args[1];
            g.nrpn_source_mask = args[2];
            if (g.nrpn_channel > 16){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            patches.set_globals(g, now_us);
            ack(source);
            return;
        }

        // Pattern data (#22). A note sequencer's step grid is far too wide
        // for NRPN - 320 bytes for a poly sequencer - so an editor writes it
        // in runs. Every byte still goes through set_param, so the same
        // validator applies and a write under a sounding note releases it
        // from the ledger like any other.
        case SYSEX_SET_PATTERN: {
            if (n < 4){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            const uint8_t node = args[0];
            const uint16_t offset = (uint16_t)(args[1] | ((uint16_t)args[2] << 7));
            const uint8_t packed_len = (uint8_t)(n - 3u);
            uint8_t bytes[SYSEX_RX_MAX];
            const size_t count = sysex::unpack(&args[3], packed_len, bytes, sizeof bytes);
            if (count == 0 && packed_len > 0){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            for (size_t i = 0; i < count; i++){
                if (patches.set_param(node, (uint16_t)(offset + i), bytes[i], now_us) != PARAM_SET_OK){
                    // A partially applied run is still better reported than
                    // hidden: the host is told which byte failed by retrying.
                    nak(source, SYSEX_ERR_BAD_ARGUMENT);
                    return;
                }
            }
            ack(source);
            return;
        }

        case SYSEX_GET_PATTERN: {
            if (n < 4){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            const uint16_t offset = (uint16_t)(args[1] | ((uint16_t)args[2] << 7));
            reply_pattern(source, args[0], offset, args[3]);
            return;
        }

        // Any target, by kind and index, so a host can read a clock setting
        // as easily as a parameter without asking for a full dump.
        case SYSEX_GET_CONTROL: {
            if (n < 4){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            const uint16_t param = (uint16_t)(args[2] | ((uint16_t)args[3] << 7));
            uint16_t value = 0;
            if (!cc.read_control(args[0], args[1], param, value)){
                nak(source, SYSEX_ERR_BAD_ARGUMENT);
                return;
            }
            uint16_t addr = 0;
            NrpnDecoder::address_of(args[0], args[1], param, addr);
            begin_reply(SYSEX_CONTROL_VALUE);
            put(args[0]);
            put(args[1]);
            put_u14(param);
            put_u14(value);
            put_u14(addr);              // the NRPN address that reaches it
            send_reply(source);
            return;
        }

        case SYSEX_SLOT_SAVE:
            if (n < 1){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            switch (patches.save_slot(args[0])){
                case APPLY_OK:        ack(source); return;
                case APPLY_TOO_LARGE: nak(source, SYSEX_ERR_TOO_LARGE); return;
                default:              nak(source, SYSEX_ERR_BAD_ARGUMENT); return;
            }

        case SYSEX_SLOT_LOAD: {
            if (n < 1){ nak(source, SYSEX_ERR_TRUNCATED); return; }
            Patch p;
            GlobalSettings g;
            const StoreError s = store.load(args[0], p, g);
            if (s == STORE_EMPTY){ nak(source, SYSEX_ERR_SLOT_EMPTY); return; }
            if (s != STORE_OK){ nak(source, SYSEX_ERR_SLOT_CORRUPT); return; }
            pending_patch = p;
            pending_globals = g;
            pending_slot = args[0];
            arm_swap(now_us);
            ack(source);
            return;
        }

        case SYSEX_SLOT_ERASE:
            if (n < 1 || args[0] >= PATCH_SLOTS){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
            store.erase(args[0]);
            ack(source);
            return;

        case SYSEX_SLOT_LIST: reply_slots(source); return;

        case SYSEX_RESTORE_DEFAULTS:
            pending_patch = default_patch();
            pending_globals = default_globals_for_patch();
            pending_slot = 0xFF;
            arm_swap(now_us);
            ack(source);
            return;

        default:
            nak(source, SYSEX_ERR_UNKNOWN_COMMAND);
            return;
    }
}

// Enumeration -----------------------------------------------------------

void SysexHandler::reply_identity(uint8_t source){
    begin_reply(SYSEX_IDENTITY);
    put(SYSEX_DEVICE_FAMILY);
    put(SYSEX_DEVICE_MEMBER);
    put_string(MMMC_VERSION);
    put_string(MMMC_GIT_REV);
    send_reply(source);
}

void SysexHandler::reply_universal_identity(uint8_t source){
    // F0 7E <dev> 06 02 <manufacturer> <family lo hi> <member lo hi> <version x4> F7
    tx_at = 0;
    tx[tx_at++] = 0xF0;
    tx[tx_at++] = SYSEX_UNIVERSAL_NON_REALTIME;
    tx[tx_at++] = device;
    tx[tx_at++] = SYSEX_GENERAL_INFORMATION;
    tx[tx_at++] = SYSEX_IDENTITY_REPLY;
    tx[tx_at++] = SYSEX_MANUFACTURER;
    put(SYSEX_DEVICE_FAMILY); put(0);
    put(SYSEX_DEVICE_MEMBER); put(0);
    put(SYSEX_PROTOCOL_VERSION); put(0); put(0); put(0);
    send_reply(source);
}

// Everything an editor needs to stop a user building a patch the module will
// reject. Read rather than hardcoded, so the two cannot drift.
void SysexHandler::reply_capabilities(uint8_t source){
    begin_reply(SYSEX_CAPABILITIES);
    put(N_NODE);
    put(MAX_IN);
    put(MAX_OUT);
    put_u14(N_PARAM);
    put(N_GATE_BUS);
    put(N_NOTE_BUS);
    put(N_CV_BUS);
    put(GPIO_N);
    put(N_MIDI_IN_NODES);
    put(N_MIDI_OUT_NODES);
    put(registry::count());
    put(PATCH_SLOTS);
    put_u14(PATCH_SLOT_BYTES);
    put(MAX_SEQUENCE_LEN);
    put(NOTE_SEQ_VOICES);
    put(DRUM_SEQ_LANES);
    put(MASTER_PPQN);
    put(MIDI_CONTROL_PORT);
    send_reply(source);
}

// One message per algorithm, straight off the compiled table, so an
// algorithm added to the firmware appears in the editor with no editor
// change and a hardcoded list cannot silently drift.
void SysexHandler::reply_algorithms(uint8_t source){
    for (uint8_t i = 0; i < registry::count(); i++){
        const AlgorithmDescriptor* d = registry::at(i);
        begin_reply(SYSEX_ALGORITHM);
        put(i);
        put(registry::count());
        put(d->id);
        put(d->n_in);
        put(d->min_in);
        put(d->n_out);
        put_u14(d->n_params);
        put(d->wants_tick ? 1 : 0);
        for (uint8_t in = 0; in < d->n_in && in < MAX_IN; in++) put((uint8_t)d->in_domain[in]);
        for (uint8_t out = 0; out < d->n_out && out < MAX_OUT; out++) put((uint8_t)d->out_domain[out]);
        put_string(d->name);
        send_reply(source);
    }
}

// Parameter descriptors, sent as the ParamGroup runs they are stored as: a
// poly sequencer's 336 parameters are two messages, not 336.
void SysexHandler::reply_param_descriptors(uint8_t source, uint8_t algorithm_id){
    const AlgorithmDescriptor* d = registry::find(algorithm_id);
    if (d == nullptr){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }

    for (uint8_t g = 0; g < d->n_param_groups; g++){
        const ParamGroup& grp = d->param_groups[g];
        for (uint16_t f = 0; f < grp.n_fields; f++){
            const ParamDescriptor& p = grp.fields[f];
            begin_reply(SYSEX_PARAM_DESC);
            put(algorithm_id);
            put(g);
            put(d->n_param_groups);
            put_u14(grp.first);
            put_u14(grp.repeat);
            put_u14(grp.n_fields);
            put_u14(f);
            put(p.min);
            put(p.max);
            put(p.def);
            put(p.kind);
            put_string(p.name);
            // Enum option names, so the editor renders a list rather than a
            // number nobody can interpret.
            const uint8_t options = (p.kind == PARAM_ENUM && p.options != nullptr)
                                  ? (uint8_t)(p.max - p.min + 1u) : 0u;
            put(options);
            for (uint8_t o = 0; o < options; o++) put_string(p.options[o]);
            send_reply(source);
        }
    }
}

// Bulk transfer ---------------------------------------------------------

void SysexHandler::reply_dump(uint8_t source){
    static uint8_t image[PATCH_SLOT_BYTES];
    size_t written = 0;
    if (patch_codec::encode(patches.active(), patches.globals(), image, sizeof image, written) != CODEC_OK){
        nak(source, SYSEX_ERR_TOO_LARGE);
        return;
    }

    uint8_t seq = 0;
    size_t at = 0;
    while (at < written){
        size_t n = written - at;
        if (n > SYSEX_CHUNK_PAYLOAD) n = SYSEX_CHUNK_PAYLOAD;

        begin_reply(SYSEX_PATCH_CHUNK_OUT);
        put(seq);
        uint8_t flags = 0;
        if (at == 0) flags |= SYSEX_CHUNK_FIRST;
        if (at + n >= written) flags |= SYSEX_CHUNK_LAST;
        put(flags);

        // Pack straight into the transmit buffer after the checksum slot,
        // then fill the checksum in: no second buffer and no copy.
        const uint16_t checksum_at = tx_at;
        put(0);
        const size_t packed = sysex::pack(&image[at], n, &tx[tx_at],
                                          (size_t)(SYSEX_TX_MAX - tx_at - 1u));
        if (packed == 0){ nak(source, SYSEX_ERR_TOO_LARGE); return; }
        tx[checksum_at] = sysex::checksum(&tx[checksum_at + 1u], packed);
        tx_at = (uint16_t)(tx_at + packed);
        send_reply(source);

        at += n;
        seq = (uint8_t)((seq + 1u) & 0x7F);
    }
}

void SysexHandler::abort_transfer(){
    receiving = false;
    staged = 0;
    next_seq = 0;
}

void SysexHandler::receive_chunk(uint8_t source, const uint8_t* args, uint16_t n, uint32_t now_us){
    if (n < 3){ nak(source, SYSEX_ERR_TRUNCATED); return; }
    const uint8_t seq = args[0];
    const uint8_t flags = args[1];
    const uint8_t checksum = args[2];
    const uint8_t* packed = &args[3];
    const uint16_t packed_len = (uint16_t)(n - 3u);

    if (sysex::checksum(packed, packed_len) != checksum){
        abort_transfer();
        nak(source, SYSEX_ERR_BAD_CHUNK);
        return;
    }

    if (flags & SYSEX_CHUNK_FIRST){
        abort_transfer();
        receiving = true;
        transfer_source = source;
        transfer_started_us = now_us;
    } else if (!receiving){
        nak(source, SYSEX_ERR_NO_TRANSFER);
        return;
    } else if (seq != next_seq){
        // A chunk out of order means chunks were lost. Abandoning the whole
        // transfer is right: half a patch is not a patch, and the active one
        // has not been touched.
        abort_transfer();
        nak(source, SYSEX_ERR_BAD_CHUNK);
        return;
    }

    transfer_started_us = now_us;
    const size_t added = sysex::unpack(packed, packed_len, &staging[staged],
                                       (size_t)(sizeof staging - staged));
    if (added == 0 && packed_len > 0){
        abort_transfer();
        nak(source, SYSEX_ERR_TOO_LARGE);
        return;
    }
    staged = (uint16_t)(staged + added);
    next_seq = (uint8_t)((seq + 1u) & 0x7F);

    if ((flags & SYSEX_CHUNK_LAST) == 0){
        ack(source);
        return;
    }

    // The whole image has arrived. Decode - which checks the magic, the
    // format version and the CRC before interpreting a byte - then validate
    // and swap. Until this point the live graph has not been touched.
    Patch decoded;
    GlobalSettings decoded_globals;
    const CodecError e = patch_codec::decode(staging, staged, decoded, decoded_globals);
    abort_transfer();
    if (e != CODEC_OK){
        nak(source, SYSEX_ERR_BAD_IMAGE);
        return;
    }
    pending_patch = decoded;
    pending_globals = decoded_globals;
    pending_slot = 0xFF;
    // A bulk load is what the user just asked for, so it is not quantised:
    // waiting for a bar boundary after a deliberate "send patch" reads as a
    // hang. Only Program Change recall and an explicit slot load are.
    if (patches.apply(decoded, decoded_globals, now_us) != APPLY_OK){
        nak(source, SYSEX_ERR_REJECTED);
        return;
    }
    ack(source);
}

// Slots -----------------------------------------------------------------

void SysexHandler::reply_slots(uint8_t source){
    begin_reply(SYSEX_SLOTS);
    put(PATCH_SLOTS);
    for (uint8_t s = 0; s < PATCH_SLOTS; s++){
        put(store.occupied(s) ? 1 : 0);
        put_u14(store.used(s));
    }
    send_reply(source);
}

void SysexHandler::reply_cc_map(uint8_t source, uint8_t slot){
    const CcMapping& m = patches.active().cc_map[slot];
    begin_reply(SYSEX_CC_MAP);
    put(slot);
    put((uint8_t)(m.source_mask & 0x7F));
    put(m.channel);
    put(m.cc);
    put(m.target_kind);
    put(m.target_index);
    put_u14(m.param);
    put_u14(m.min);
    put((uint8_t)(m.flags | ((m.source_mask & 0x80) ? 0x40u : 0u)));
    put_u14(m.max);
    send_reply(source);
}

// Pattern data (#22). N_PARAM is 336 because the poly and drum sequencers
// carry a 32-step grid, so a "pattern blob" needs no arena of its own: the
// bytes are already in NodeConfig::params and travel with the patch. This is
// the addressed read of a run of them.
void SysexHandler::reply_pattern(uint8_t source, uint8_t node, uint16_t offset, uint16_t length){
    const AlgorithmDescriptor* d = mm.node_descriptor(node);
    if (d == nullptr){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
    if (offset >= d->n_params){ nak(source, SYSEX_ERR_BAD_ARGUMENT); return; }
    if (length == 0 || length > SYSEX_CHUNK_PAYLOAD) length = SYSEX_CHUNK_PAYLOAD;
    if (offset + length > d->n_params) length = (uint16_t)(d->n_params - offset);

    uint8_t bytes[SYSEX_CHUNK_PAYLOAD];
    for (uint16_t i = 0; i < length; i++){
        uint8_t v = 0;
        mm.get_node_param(node, (uint16_t)(offset + i), v);
        bytes[i] = v;
    }

    begin_reply(SYSEX_PATTERN);
    put(node);
    put_u14(offset);
    put((uint8_t)length);
    const size_t packed = sysex::pack(bytes, length, &tx[tx_at], (size_t)(SYSEX_TX_MAX - tx_at - 1u));
    if (packed == 0){ nak(source, SYSEX_ERR_TOO_LARGE); return; }
    tx_at = (uint16_t)(tx_at + packed);
    send_reply(source);
}

// Quantised swap --------------------------------------------------------

uint32_t SysexHandler::swap_boundary() const {
    const uint32_t now = mm.clock().count();
    const uint32_t quarter = CLOCK_SUBTICKS_PER_QUARTER;
    const uint32_t unit = (timing == SWAP_NEXT_BAR) ? quarter * BEATS_PER_BAR : quarter;
    return ((now / unit) + 1u) * unit;
}

void SysexHandler::arm_swap(uint32_t now_us){
    if (timing == SWAP_IMMEDIATE || !mm.clock().running()){
        // Nothing to wait for: with the clock stopped, "the next bar" never
        // arrives, and a recall that never happened is worse than one that
        // glitched.
        patches.apply(pending_patch, pending_globals, now_us);
        cc.reset();
        if (pending_slot != 0xFF) notify(SYSEX_EVENT_PROGRAM_CHANGE, pending_slot);
        pending = false;
        return;
    }
    pending_at = swap_boundary();
    pending = true;
}

void SysexHandler::service(uint32_t now_us){
    // A transfer whose host died must not leave the module waiting for ever,
    // and must not touch the active patch on its way out.
    if (receiving && (uint32_t)(now_us - transfer_started_us) > SYSEX_TRANSFER_TIMEOUT_US){
        abort_transfer();
        leds.error(now_us);
        error = SYSEX_ERR_TRUNCATED;
        notify(SYSEX_EVENT_ERROR, SYSEX_ERR_TRUNCATED);
    }

    // A learn that captured a controller is reported once, so the editor
    // updates without polling.
    uint8_t learned = 0;
    if (cc.take_learn_result(learned)) notify(SYSEX_EVENT_CC_LEARNED, learned);

    if (pending && mm.clock().count() >= pending_at){
        patches.apply(pending_patch, pending_globals, now_us);
        cc.reset();
        pending = false;
        notify(pending_slot != 0xFF ? SYSEX_EVENT_PROGRAM_CHANGE : SYSEX_EVENT_PATCH_APPLIED,
               pending_slot != 0xFF ? pending_slot : 0);
    }
}

// Program Change --------------------------------------------------------

bool SysexHandler::program_change(uint8_t source, uint8_t channel, uint8_t program, uint32_t now_us){
    const GlobalSettings& g = patches.globals();
    // Off by default, and both the port and the channel are configurable.
    // Otherwise a Program Change intended for a downstream synth silently
    // switches the user's patch - the most likely field complaint in the
    // whole feature.
    if (!g.pc_enabled) return false;
    if (g.pc_source_mask != 0 && (g.pc_source_mask & source) == 0) return false;
    if (g.pc_channel != 0 && g.pc_channel != channel) return false;
    if (program >= PATCH_SLOTS) return false;

    Patch p;
    GlobalSettings recalled;
    if (store.load(program, p, recalled) != STORE_OK){
        leds.error(now_us);
        return true;                      // addressed to us; it just failed
    }
    pending_patch = p;
    pending_globals = recalled;
    pending_slot = program;
    timing = g.pc_quantise;
    arm_swap(now_us);
    return true;
}
