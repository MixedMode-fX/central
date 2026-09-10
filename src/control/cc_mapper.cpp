#include "control/cc_mapper.h"
#include "node/registry.h"
#include "hal/midi_types.h"

CcMapper::CcMapper(PatchManager& manager, MixedModeMaster& master) :
    patches(manager), mm(master), knobs(),
    write_count(0), refuse_count(0),
    learn_template(unused_mapping()), learn_armed_us(0),
    learn_slot(0), learn_result_slot(0), learn_armed(false), learn_result(false)
{
    reset();
}

void CcMapper::reset(){
    for (uint8_t i = 0; i < N_CC_MAP; i++){
        knobs[i] = Knob{0, 0, 0, false, false, false, false, 0, 0, 0, false};
    }
}

// Ranges come from #20's descriptors wherever there is one, so "the sub-range
// the knob sweeps" means the same thing for a Euclidean pulse count and for
// the tempo, and nothing is hardcoded per target.
bool CcMapper::target_range(const CcMapping& m, uint16_t& lo, uint16_t& hi) const {
    return target_range(m.target_kind, m.target_index, m.param, lo, hi);
}

bool CcMapper::target_range(uint8_t target_kind, uint8_t target_index, uint16_t param,
                            uint16_t& lo, uint16_t& hi) const {
    switch (target_kind){
        case CC_TARGET_NODE: {
            const AlgorithmDescriptor* d = mm.node_descriptor(target_index);
            if (d == nullptr) return false;
            const ParamDescriptor* p = registry::param(*d, param);
            if (p == nullptr) return false;
            lo = p->min;
            hi = p->max;
            return true;
        }
        case CC_TARGET_CLOCK:
            switch (param){
                case CC_CLOCK_TEMPO:  lo = CLOCK_MIN_BPM; hi = CLOCK_MAX_BPM; return true;
                case CC_CLOCK_SOURCE: lo = MasterClock::CLOCK_INTERNAL; hi = MasterClock::CLOCK_MIDI; return true;
                case CC_CLOCK_PPQN:   lo = 1; hi = 48; return true;
                default: return false;
            }
        case CC_TARGET_TRANSPORT:
            // Momentary: there is no range to sweep, only a threshold.
            lo = 0; hi = 1;
            return param < CC_TRANSPORT_TARGETS;
        default:
            return false;
    }
}

uint16_t CcMapper::scale_in(uint32_t position, uint32_t full, uint16_t lo, uint16_t hi){
    if (hi <= lo) return lo;
    if (full == 0) return lo;
    if (position > full) position = full;
    const uint32_t span = (uint32_t)(hi - lo);
    // Rounded rather than truncated, so the top of the travel reaches `hi`
    // and the middle of a three-value range is the middle value.
    return (uint16_t)(lo + ((position * span + full / 2u) / full));
}

int8_t CcMapper::relative_delta(uint8_t flags, uint8_t value){
    // An encoder sending relative data interpreted as absolute makes a
    // parameter jump to the extremes, and the user has no way to diagnose it -
    // which is why all three encodings are supported rather than one.
    switch (flags & CC_RELATIVE_MASK){
        case CC_RELATIVE_TWOS:
            // 1..63 up, 127..65 down (two's complement in seven bits).
            if (value == 0 || value == 64) return 0;
            return (value < 64) ? (int8_t)value : (int8_t)((int16_t)value - 128);
        case CC_RELATIVE_SIGNED:
            // 1..63 up, 65..127 down (bit 6 is the sign).
            if (value == 0 || value == 64) return 0;
            return (value < 64) ? (int8_t)value : (int8_t)(-(int16_t)(value - 64));
        case CC_RELATIVE_OFFSET64:
            // 64 is no movement; above is up, below is down.
            return (int8_t)((int16_t)value - 64);
        default:
            return 0;
    }
}

bool CcMapper::observe(uint8_t source, uint8_t channel, uint8_t cc, uint8_t value, uint32_t now_us){
    // Channel mode messages (120..127) are not controllers; binding one would
    // mean a patch could swallow All Notes Off.
    if (cc > 119) return false;

    // Learn, before matching: the whole point is to bind a CC that has no
    // mapping yet.
    if (learn_armed){
        if ((uint32_t)(now_us - learn_armed_us) > LEARN_TIMEOUT_US){
            learn_cancel();
        } else if (source != MIDI_CONTROL_PORT){
            // Ignore the reserved control cable while armed: a learn that
            // bound to its own control port is a trap.
            CcMapping m = learn_template;
            m.source_mask = source;
            m.channel = channel;
            m.cc = cc;
            patches.begin_edit();
            patches.staging().cc_map[learn_slot] = m;
            if (patches.commit_cc_map(learn_slot, now_us) == APPLY_OK){
                knobs[learn_slot] = Knob{0, 0, 0, false, false, false, false, 0, 0, 0, false};
                learn_result_slot = learn_slot;
                learn_result = true;
            }
            learn_armed = false;
            // The CC that did the binding is consumed: it was a gesture, not
            // a value, and passing it on would move the parameter it just
            // bound by whatever the knob happened to be at.
            return true;
        }
    }

    const Patch& p = patches.active();
    bool consumed = false;

    for (uint8_t i = 0; i < N_CC_MAP; i++){
        const CcMapping& m = p.cc_map[i];
        if (m.source_mask == 0) continue;
        if ((m.source_mask & source) == 0) continue;
        if (m.channel != 0 && m.channel != channel) continue;

        Knob& k = knobs[i];

        // A 14-bit pair: CC n is the MSB, CC n+32 the LSB. An LSB on its own
        // is remembered and nothing else; the write happens on the MSB, using
        // the last LSB seen, which is the standard behaviour and costs one
        // message of latency when a controller sends LSB first.
        if ((m.flags & CC_FOURTEEN_BIT) && m.cc < 32 && cc == (uint8_t)(m.cc + 32u)){
            k.last_lsb = value;
            k.have_lsb = true;
            if ((m.flags & CC_PASS_THROUGH) == 0) consumed = true;
            continue;
        }
        if (m.cc != cc) continue;

        if (m.flags & CC_FOURTEEN_BIT){
            // A lone MSB with no LSB yet is applied as if the LSB were zero,
            // rather than stalling: a knob that does nothing until its
            // partner arrives looks broken.
            k.pending = (uint16_t)(((uint16_t)value << 7) | (k.have_lsb ? k.last_lsb : 0u));
        } else {
            k.pending = value;
        }
        k.has_pending = true;
        // At 32 entries a linear scan per event is a few hundred nanoseconds
        // and simpler than an index; measured against the jitter budget (#4)
        // rather than optimised on a hunch.
        if ((m.flags & CC_PASS_THROUGH) == 0) consumed = true;
    }
    return consumed;
}

void CcMapper::apply(uint32_t now_us){
    for (uint8_t i = 0; i < N_CC_MAP; i++){
        if (!knobs[i].has_pending) continue;
        apply_one(i, now_us);
        knobs[i].has_pending = false;
    }
    if (learn_armed && (uint32_t)(now_us - learn_armed_us) > LEARN_TIMEOUT_US) learn_cancel();
}

void CcMapper::apply_one(uint8_t index, uint32_t now_us){
    const CcMapping& m = patches.active().cc_map[index];
    if (m.source_mask == 0) return;
    Knob& k = knobs[index];

    uint16_t lo = 0, hi = 0;
    if (!target_range(m, lo, hi)){ refuse_count++; return; }
    // The mapping's own sub-range, clamped into what the target actually
    // accepts, so a stale mapping cannot drive a parameter out of range.
    uint16_t range_lo = (m.min == 0 && m.max == 0) ? lo : m.min;
    uint16_t range_hi = (m.min == 0 && m.max == 0) ? hi : m.max;
    if (range_lo < lo) range_lo = lo;
    if (range_hi > hi) range_hi = hi;
    if (range_hi < range_lo) range_hi = range_lo;

    const uint8_t raw = (uint8_t)(k.pending & 0x7F);

    // Transport targets are momentary actions on the way up, not values.
    if (m.target_kind == CC_TARGET_TRANSPORT){
        const bool level = raw >= 64;
        const bool rising = level && !k.last_switch;
        k.last_switch = level;
        if (rising) write_target(m, 1, now_us);
        return;
    }

    const uint16_t full = (m.flags & CC_FOURTEEN_BIT) ? 16383u : 127u;

    // Relative encoders sidestep takeover entirely, which is why they are the
    // mode worth recommending: there is no physical position to reconcile.
    if ((m.flags & CC_RELATIVE_MASK) != CC_ABSOLUTE){
        const int8_t delta = relative_delta(m.flags, raw);
        if (delta == 0) return;
        int32_t next = (int32_t)k.last_applied + delta;
        if (!k.engaged){
            // The first move starts from where the target actually is, not
            // from zero.
            uint16_t current = range_lo;
            if (m.target_kind == CC_TARGET_NODE){
                uint8_t v = 0;
                if (mm.get_node_param(m.target_index, m.param, v)) current = v;
            } else if (m.target_kind == CC_TARGET_CLOCK && m.param == CC_CLOCK_TEMPO){
                current = mm.clock().bpm();
            }
            k.engaged = true;
            next = (int32_t)current + delta;
        }
        if (next < range_lo) next = range_lo;
        if (next > range_hi) next = range_hi;
        write_target(m, (uint16_t)next, now_us);
        k.last_applied = (uint16_t)next;
        return;
    }

    const uint16_t want = scale_in(k.pending, full, range_lo, range_hi);

    switch (m.flags & CC_TAKEOVER_MASK){
        case CC_TAKEOVER_PICKUP: {
            // Ignore the knob until it crosses the value the target is
            // actually at. Correct, and confusing the first time a knob does
            // nothing - which is why it is not the default.
            if (!k.engaged){
                uint16_t current = k.last_applied;
                if (m.target_kind == CC_TARGET_NODE){
                    uint8_t v = 0;
                    if (mm.get_node_param(m.target_index, m.param, v)) current = v;
                } else if (m.target_kind == CC_TARGET_CLOCK && m.param == CC_CLOCK_TEMPO){
                    current = mm.clock().bpm();
                }
                const bool crossed = k.have_physical
                    && ((k.last_applied <= current && want >= current)
                     || (k.last_applied >= current && want <= current));
                k.last_applied = want;
                k.have_physical = true;
                k.physical = raw;
                if (!crossed) return;
                k.engaged = true;
            }
            break;
        }
        case CC_TAKEOVER_SCALE: {
            // On the first move, scale the remaining travel so the knob still
            // reaches both ends. The best of the three, and the most code.
            //
            // The anchor - where the knob was, and what the target was at -
            // is frozen once and everything afterwards is measured from it.
            // Re-reading the target on every message instead would compound:
            // the knob would reach the top and then be unable to come back
            // down.
            const uint32_t pos = k.pending;
            if (!k.engaged){
                if (!k.have_physical){
                    // The first message only says where the knob is; acting
                    // on it would be a jump, which is the mode the user did
                    // not choose.
                    k.have_physical = true;
                    k.physical = raw;
                    k.anchor_pos = (uint16_t)pos;
                    k.last_applied = want;
                    return;
                }
                uint16_t current = k.last_applied;
                if (m.target_kind == CC_TARGET_NODE){
                    uint8_t v = 0;
                    if (mm.get_node_param(m.target_index, m.param, v)) current = v;
                } else if (m.target_kind == CC_TARGET_CLOCK && m.param == CC_CLOCK_TEMPO){
                    current = mm.clock().bpm();
                }
                if (current < range_lo) current = range_lo;
                if (current > range_hi) current = range_hi;
                k.engaged = true;
                k.anchor_value = current;
            }

            // Two straight lines meeting at the anchor: below it the knob
            // sweeps [range_lo, anchor_value], above it [anchor_value,
            // range_hi]. Continuous, monotonic, and it reaches both ends.
            const uint32_t anchor = k.anchor_pos;
            uint16_t scaled;
            if (pos >= anchor){
                const uint32_t travel = full - anchor;
                scaled = travel == 0 ? range_hi
                       : (uint16_t)(k.anchor_value
                                    + ((uint32_t)(range_hi - k.anchor_value) * (pos - anchor) + travel / 2u) / travel);
            } else {
                scaled = anchor == 0 ? range_lo
                       : (uint16_t)(range_lo
                                    + ((uint32_t)(k.anchor_value - range_lo) * pos + anchor / 2u) / anchor);
            }
            if (scaled < range_lo) scaled = range_lo;
            if (scaled > range_hi) scaled = range_hi;
            write_target(m, scaled, now_us);
            k.last_applied = scaled;
            k.physical = raw;
            return;
        }
        default:
            // Jump: take the value immediately. The default, because it is
            // the only mode that always responds, and a silent knob is a
            // worse first impression than a jump.
            break;
    }

    write_target(m, want, now_us);
    k.last_applied = want;
    k.have_physical = true;
    k.physical = raw;
}

void CcMapper::write_target(const CcMapping& m, uint16_t value, uint32_t now_us){
    write_control(m.target_kind, m.target_index, m.param, value, now_us);
}

// The one applier. A mapped CC and an NRPN both end here, so they produce
// identical results and are rejected identically when out of range.
bool CcMapper::write_control(uint8_t kind, uint8_t index, uint16_t param,
                             uint16_t value, uint32_t now_us, bool transient){
    switch (kind){
        case CC_TARGET_NODE: {
            const ParamError e = transient
                ? patches.modulate_param(index, param, (uint8_t)value)
                : patches.set_param(index, param, (uint8_t)value, now_us);
            if (e == PARAM_SET_OK){
                write_count++;
                return true;
            }
            refuse_count++;
            return false;
        }

        case CC_TARGET_CLOCK: {
            GlobalSettings g = patches.globals();
            switch (param){
                case CC_CLOCK_TEMPO:  g.bpm = value; break;
                case CC_CLOCK_SOURCE: g.clock_source = (uint8_t)value; break;
                case CC_CLOCK_PPQN:   g.cv_ppqn = (uint8_t)value; break;
                default: refuse_count++; return false;
            }
            patches.set_globals(g, now_us, !transient);
            write_count++;
            return true;
        }

        case CC_TARGET_TRANSPORT:
            switch (param){
                case CC_TRANSPORT_START:    mm.clock().start(); break;
                case CC_TRANSPORT_STOP:     mm.clock().stop(); break;
                case CC_TRANSPORT_CONTINUE: mm.clock().resume(); break;
                case CC_TRANSPORT_TAP:      mm.clock().tap(now_us); break;
                default: refuse_count++; return false;
            }
            write_count++;
            return true;

        default:
            refuse_count++;
            return false;
    }
}

bool CcMapper::read_control(uint8_t kind, uint8_t index, uint16_t param, uint16_t& value_out) const {
    switch (kind){
        case CC_TARGET_NODE: {
            uint8_t v = 0;
            if (!mm.get_node_param(index, param, v)) return false;
            value_out = v;
            return true;
        }
        case CC_TARGET_CLOCK:
            switch (param){
                case CC_CLOCK_TEMPO:  value_out = mm.clock().bpm(); return true;
                case CC_CLOCK_SOURCE: value_out = mm.clock().source(); return true;
                case CC_CLOCK_PPQN:   value_out = mm.clock().cv_ppqn(); return true;
                default: return false;
            }
        case CC_TARGET_TRANSPORT:
            if (param >= CC_TRANSPORT_TARGETS) return false;
            value_out = mm.clock().running() ? 1u : 0u;
            return true;
        default:
            return false;
    }
}

// Learn ---------------------------------------------------------------------

void CcMapper::learn_arm(uint8_t slot, uint8_t target_kind, uint8_t target_index,
                         uint16_t param, uint32_t now_us){
    if (slot >= N_CC_MAP) return;
    learn_template = unused_mapping();
    learn_template.target_kind = target_kind;
    learn_template.target_index = target_index;
    learn_template.param = param;
    learn_slot = slot;
    learn_armed = true;
    learn_armed_us = now_us;
}

void CcMapper::learn_cancel(){ learn_armed = false; }

bool CcMapper::take_learn_result(uint8_t& slot_out){
    if (!learn_result) return false;
    learn_result = false;
    slot_out = learn_result_slot;
    return true;
}
