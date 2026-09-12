#include "midi/global_key.h"

// Constant-initialised, so there is no static initialisation order to get
// wrong: a node constructed before any patch is loaded reads chromatic on C.
namespace {
    uint8_t current_id = SCALE_CHROMATIC;
    uint8_t current_root = 0;
    uint8_t current_octave = global_key::DEFAULT_OCTAVE;
    uint16_t current_mask = 0x0FFF;
}

void global_key::set(uint8_t scale_id, uint8_t root_pitch_class, uint8_t register_octave){
    set_scale(scale_id);
    set_root(root_pitch_class);
    set_octave(register_octave);
}

void global_key::set_octave(uint8_t register_octave){
    // Zero is a preset byte nobody set, and the module's rule is that such a
    // byte means the default (node/param.h).
    current_octave = register_octave == 0 ? DEFAULT_OCTAVE
                   : (register_octave > MAX_OCTAVE ? MAX_OCTAVE : register_octave);
}

void global_key::set_scale(uint8_t scale_id){
    const uint16_t m = scale_mask(scale_id);
    // SCALE_NONE is a zeroed byte and not a scale; it reads as chromatic,
    // which is what the module played before there was a key at all.
    current_id = m ? scale_id : (uint8_t)SCALE_CHROMATIC;
    current_mask = m ? m : (uint16_t)0x0FFF;
}

void global_key::set_root(uint8_t root_pitch_class){
    current_root = (uint8_t)(root_pitch_class % 12u);
}

uint8_t global_key::id(){ return current_id; }
uint8_t global_key::root(){ return current_root; }
uint8_t global_key::octave(){ return current_octave; }
uint16_t global_key::mask(){ return current_mask; }

uint8_t global_key::tonic(uint8_t node_octave){
    const uint8_t octave_used = node_octave == 0 ? current_octave
                              : (node_octave > MAX_OCTAVE ? MAX_OCTAVE : node_octave);
    int16_t note = (int16_t)octave_used * 12 + (int16_t)current_root;
    // The top octave cannot hold every pitch class, and a key that silently
    // became a different note would be worse than one an octave lower.
    while (note > 127) note -= 12;
    return (uint8_t)note;
}
