#include "control/mod_matrix.h"

ModMatrix::ModMatrix(PatchManager& manager, CcMapper& mapper) :
    patches(manager), cc(mapper), lanes(), write_count(0), refuse_count(0)
{
    reset();
}

void ModMatrix::reset(){
    for (uint8_t i = 0; i < N_MOD_ROUTE; i++){
        lanes[i] = Lane{unused_route(), 0, 0, false};
    }
}

bool ModMatrix::same_route(const ModRoute& a, const ModRoute& b){
    return a.bus == b.bus
        && a.target_kind == b.target_kind
        && a.target_index == b.target_index
        && a.param == b.param
        && a.min == b.min
        && a.max == b.max
        && a.depth == b.depth
        && a.flags == b.flags;
}

bool ModMatrix::last_written(uint8_t slot, uint16_t& value_out) const {
    if (slot >= N_MOD_ROUTE || !lanes[slot].have_written) return false;
    value_out = lanes[slot].written;
    return true;
}

void ModMatrix::read_signal(const ModRoute& route, int16_t cv,
                            int32_t& position, int32_t& swing){
    if (route.flags & MOD_BIPOLAR){
        // Centred on zero: the swing is the signal, and the position is the
        // signal moved onto the same 0 .. CV_MAX scale a unipolar one is
        // already on, so absolute mode reads both the same way.
        //
        // **The swing is not clamped to the bipolar range.** "Full depth
        // covers half the range each way" is a statement about a signal that
        // really is centred on zero, not a limit imposed on one that is not:
        // clamping at CV_HALF would flatten the whole top half of a *unipolar*
        // source read through a bipolar route onto one value, which is what a
        // route made by dragging any of SampleHold, Slew or a unipolar LFO
        // onto a block would be. The result is clamped into the target's range
        // below, which is the limit that matters.
        swing = cv;
        position = cv + CV_HALF;
    } else {
        // Unipolar: an offset route only ever pushes the parameter up, which
        // is what a signal that never goes below zero should do to it.
        const int32_t level = cv_clamp_unipolar(cv);
        swing = level;
        position = level;
    }
    if (route.flags & MOD_INVERT){
        position = CV_MAX - position;
        swing = -swing;
    }
    if (position < 0) position = 0;
    if (position > CV_MAX) position = CV_MAX;
}

void ModMatrix::apply(const BusManager& buses, uint32_t now_us){
    for (uint8_t i = 0; i < N_MOD_ROUTE; i++) apply_one(i, buses, now_us);
}

void ModMatrix::apply_one(uint8_t slot, const BusManager& buses, uint32_t now_us){
    const ModRoute& route = patches.active().mod_map[slot];
    Lane& lane = lanes[slot];

    if (!same_route(lane.tracked, route)){
        // The route changed under us. Whatever centre this lane was holding
        // belonged to a different binding, so it goes rather than being
        // applied to the new one.
        lane = Lane{route, 0, 0, false};
    }
    if (route.bus == NO_BUS || route.bus >= N_CV_BUS) return;
    if (route.depth == 0) return;              // a silent route costs a compare

    uint16_t lo = 0, hi = 0;
    if (!cc.target_range(route.target_kind, route.target_index, route.param, lo, hi)){
        refuse_count++;
        return;
    }
    // The route's own sub-range, clamped into what the target actually
    // accepts, exactly as a controller binding's is: a stale route cannot
    // drive a parameter out of range.
    uint16_t range_lo = (route.min == 0 && route.max == 0) ? lo : route.min;
    uint16_t range_hi = (route.min == 0 && route.max == 0) ? hi : route.max;
    if (range_lo < lo) range_lo = lo;
    if (range_hi > hi) range_hi = hi;
    if (range_hi < range_lo) range_hi = range_lo;
    const int32_t span = (int32_t)range_hi - (int32_t)range_lo;

    int32_t position = 0;
    int32_t swing = 0;
    read_signal(route, buses.cv_read(route.bus), position, swing);

    int32_t want;
    if ((route.flags & MOD_MODE_MASK) == MOD_OFFSET){
        uint16_t current = 0;
        if (!cc.read_control(route.target_kind, route.target_index, route.param, current)){
            refuse_count++;
            return;
        }
        // A target that is not where this lane left it has been moved by
        // something else - a knob, the editor, a preset recall - and that new
        // value is the centre from here on. Re-reading it unconditionally
        // instead would compound: the modulation would walk the parameter
        // away in whichever direction it happened to be pushing.
        if (!lane.have_written || current != lane.written) lane.centre = current;

        // A signal that really is centred on zero covers half the range each
        // way at full depth, so a bipolar modulator reaches both ends of the
        // range and no further. A signal that only ever goes one way - a
        // unipolar LFO, a sample and hold - covers that half in that
        // direction, which is the honest reading of it and not a clip.
        const int32_t delta = (swing * route.depth / 255) * span / CV_FULL;
        want = (int32_t)lane.centre + delta;
    } else {
        // Absolute: the position *is* the value, depth scaling how much of
        // the range it reaches. Rounded rather than truncated, so the top of
        // the signal reaches range_hi.
        const int32_t scaled = position * route.depth / 255;
        want = range_lo + (scaled * span + CV_MAX / 2) / CV_MAX;
    }

    if (want < range_lo) want = range_lo;
    if (want > range_hi) want = range_hi;

    if (lane.have_written && (uint16_t)want == lane.written) return;
    // Transient: the node takes the value, the patch image and the store do
    // not. A modulator writes every pass for as long as the patch runs, and
    // treating that as an edit would be tens of thousands of EEPROM writes a
    // day and a preset that saved whatever phase the LFO was at.
    if (cc.write_control(route.target_kind, route.target_index, route.param,
                         (uint16_t)want, now_us, true)){
        write_count++;
        lane.written = (uint16_t)want;
        lane.have_written = true;
    } else {
        refuse_count++;
    }
}
