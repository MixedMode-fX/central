#include "midi/global_scale.h"

// Constant-initialised, so there is no static initialisation order to get
// wrong: a node constructed before any patch is loaded reads chromatic.
namespace {
    uint8_t current_id = SCALE_CHROMATIC;
    uint8_t current_root = 0;
    uint8_t current_octave = 0;        // 0: the key names no register
    uint16_t current_mask = 0x0FFF;
    // The highest octave whose root can be a note at all: 10 x 12 is 120, and
    // above that a pitch class of 8 or more has nowhere to sit.
    constexpr uint8_t MAX_OCTAVE = 10;
}

void global_scale::set(uint8_t scale_id, uint8_t root_pitch_class, uint8_t root_octave){
    const uint16_t m = scale_mask(scale_id);
    // The global scale is what SCALE_GLOBAL points at, so it cannot be
    // SCALE_GLOBAL itself; an empty mask reads as chromatic.
    current_id = m ? scale_id : (uint8_t)SCALE_CHROMATIC;
    current_mask = m ? m : (uint16_t)0x0FFF;
    current_root = (uint8_t)(root_pitch_class % 12u);
    current_octave = root_octave > MAX_OCTAVE ? MAX_OCTAVE : root_octave;
}

uint8_t global_scale::id(){ return current_id; }
uint8_t global_scale::root(){ return current_root; }
uint8_t global_scale::octave(){ return current_octave; }
uint16_t global_scale::mask(){ return current_mask; }

uint8_t global_scale::root_note(){
    if (current_octave == 0) return NO_ROOT_NOTE;
    int16_t note = (int16_t)current_octave * 12 + (int16_t)current_root;
    // The top octave cannot hold every pitch class, and a key that silently
    // became a different note would be worse than one an octave lower.
    while (note > 127) note -= 12;
    return (uint8_t)note;
}
