#include "control/macros.h"
#include "control/control_sum.h"

Macros::Macros() : pos(), on() {
    reset();
}

void Macros::reset(){
    for (uint8_t i = 0; i < N_MACRO; i++){
        pos[i] = 0;
        on[i] = false;
    }
}

bool Macros::set(uint8_t index, uint8_t value){
    if (index >= N_MACRO) return false;
    pos[index] = value;
    on[index] = true;
    return true;
}

uint8_t Macros::value(uint8_t index) const {
    return index < N_MACRO ? pos[index] : 0;
}

bool Macros::engaged(uint8_t index) const {
    return index < N_MACRO ? on[index] : false;
}

int32_t Macros::contribution(const MacroDest& dest, uint8_t pos_now){
    if (dest.src_hi <= dest.src_lo){
        // No width: a step. Reads as "from here on", and costs no divide.
        return pos_now >= dest.src_lo ? (int32_t)dest.depth : 0;
    }
    if (pos_now <= dest.src_lo) return 0;
    // Held past the top of the window rather than released. A destination
    // that rises and falls back is two entries with opposite depths; see
    // node/patch.h for why that is the way round it is.
    if (pos_now >= dest.src_hi) return (int32_t)dest.depth;
    const int32_t travelled = (int32_t)pos_now - (int32_t)dest.src_lo;
    const int32_t width = (int32_t)dest.src_hi - (int32_t)dest.src_lo;
    return (int32_t)dest.depth * travelled / width;
}

void Macros::expand(const Patch& patch, ControlSum& sum) const {
    for (uint8_t i = 0; i < N_MACRO_DEST; i++){
        const MacroDest& dest = patch.macro_dest[i];
        if (dest.macro == MACRO_NONE || dest.macro >= N_MACRO) continue;
        // An untouched macro is silent, not zero: its destinations keep
        // whatever the patch dialled in.
        if (!on[dest.macro]) continue;
        // A zero contribution is offered like any other, and deliberately.
        // Dropping it would leave the target out of the sum, and a target
        // nobody asks for keeps whatever was last written to it - so sweeping
        // a macro back down below a window would strand the parameter at the
        // bottom of that window instead of returning it to the dialled value.
        // Asking for zero is what asks for the anchor.
        sum.add(dest.target_kind, dest.target_index, dest.param,
                contribution(dest, pos[dest.macro]));
    }
}
