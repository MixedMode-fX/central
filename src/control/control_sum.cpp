#include "control/control_sum.h"
#include "control/cc_mapper.h"

ControlSum::ControlSum(CcMapper& mapper) :
    cc(mapper), targets(), n_targets(0), write_count(0), refuse_count(0)
{
    reset();
}

void ControlSum::reset(){
    n_targets = 0;
    for (uint8_t i = 0; i < CAPACITY; i++) targets[i] = Target{};
}

int16_t ControlSum::find(uint8_t kind, uint8_t index, uint16_t param) const {
    for (uint8_t i = 0; i < n_targets; i++){
        const Target& t = targets[i];
        if (t.kind == kind && t.index == index && t.param == param) return (int16_t)i;
    }
    return -1;
}

void ControlSum::begin(){
    for (uint8_t i = 0; i < n_targets; i++){
        targets[i].offset = 0;
        targets[i].asked = false;
    }
}

void ControlSum::add(uint8_t kind, uint8_t index, uint16_t param, int32_t offset){
    const int16_t at = find(kind, index, param);
    if (at >= 0){
        Target& t = targets[at];
        t.offset += offset;
        t.asked = true;
        return;
    }
    if (n_targets >= CAPACITY){
        // Unreachable while every contributor names at most one target, and
        // counted rather than asserted so a future contributor that breaks
        // that shows up as a refusal instead of silently winning.
        refuse_count++;
        return;
    }
    Target& t = targets[n_targets++];
    t = Target{};
    t.kind = kind;
    t.index = index;
    t.param = param;
    t.offset = offset;
    t.asked = true;
}

void ControlSum::commit(uint32_t now_us){
    for (uint8_t i = 0; i < n_targets; i++){
        if (targets[i].asked) commit_one(targets[i], now_us);
    }
    compact();
}

void ControlSum::commit_one(Target& t, uint32_t now_us){
    uint16_t current = 0;
    if (!cc.read_control(t.kind, t.index, t.param, current)){
        refuse_count++;
        t.asked = false;   // a target that does not answer is not a target
        return;
    }
    // Somebody else moved it: that is the anchor from here on. See the note
    // in the header on why this is not re-read unconditionally.
    if (!t.have_written || current != t.written) t.anchor = current;

    uint16_t lo = 0, hi = 0;
    if (!cc.target_range(t.kind, t.index, t.param, lo, hi)){
        refuse_count++;
        t.asked = false;
        return;
    }

    int32_t want = (int32_t)t.anchor + t.offset;
    t.clipped = want < (int32_t)lo || want > (int32_t)hi;
    if (want < (int32_t)lo) want = lo;
    if (want > (int32_t)hi) want = hi;
    t.value = (uint16_t)want;

    if (t.have_written && t.value == t.written) return;
    // Transient: the node takes the value, the patch image and the store do
    // not. A macro held for an hour must not be an hour of EEPROM writes -
    // patch/patch_manager.h says this at length.
    if (cc.write_control(t.kind, t.index, t.param, t.value, now_us, true)){
        write_count++;
        t.written = t.value;
        t.have_written = true;
    } else {
        refuse_count++;
    }
}

void ControlSum::compact(){
    uint8_t kept = 0;
    for (uint8_t i = 0; i < n_targets; i++){
        if (!targets[i].asked) continue;
        if (kept != i) targets[kept] = targets[i];
        kept++;
    }
    for (uint8_t i = kept; i < n_targets; i++) targets[i] = Target{};
    n_targets = kept;
}

bool ControlSum::lookup(uint8_t kind, uint8_t index, uint16_t param,
                        uint16_t& anchor_out, uint16_t& value_out, bool& clipped_out) const {
    const int16_t at = find(kind, index, param);
    if (at < 0) return false;
    anchor_out = targets[at].anchor;
    value_out = targets[at].value;
    clipped_out = targets[at].clipped;
    return true;
}
