#include "midi/global_scale.h"

// Constant-initialised, so there is no static initialisation order to get
// wrong: a node constructed before any patch is loaded reads chromatic.
namespace {
    uint8_t current_id = SCALE_CHROMATIC;
    uint8_t current_root = 0;
    uint16_t current_mask = 0x0FFF;
}

void global_scale::set(uint8_t scale_id, uint8_t root_pitch_class){
    const uint16_t m = scale_mask(scale_id);
    // The global scale is what SCALE_GLOBAL points at, so it cannot be
    // SCALE_GLOBAL itself; an empty mask reads as chromatic.
    current_id = m ? scale_id : (uint8_t)SCALE_CHROMATIC;
    current_mask = m ? m : (uint16_t)0x0FFF;
    current_root = (uint8_t)(root_pitch_class % 12u);
}

uint8_t global_scale::id(){ return current_id; }
uint8_t global_scale::root(){ return current_root; }
uint16_t global_scale::mask(){ return current_mask; }
