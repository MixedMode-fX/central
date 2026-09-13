#include "algorithm/midi/transpose.h"
#include "node/registry.h"
#include "midi/note_event.h"

static const Domain IN[1] = {Domain::Note};
static const Domain OUT[1] = {Domain::Note};

static const ParamDescriptor PARAMS[2] = {
    {"semitones", PARAM_CENTRE - Transpose::MAX_SEMITONES, PARAM_CENTRE + Transpose::MAX_SEMITONES,
                  PARAM_CENTRE, PARAM_CENTRED, nullptr},
    {"octaves",   PARAM_CENTRE - Transpose::MAX_OCTAVES,   PARAM_CENTRE + Transpose::MAX_OCTAVES,
                  PARAM_CENTRE, PARAM_CENTRED, nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 2, PARAMS}};

static const char* const IN_NAMES[1] = {"notes in"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor Transpose::descriptor = {
    ALGO_TRANSPOSE, "Transpose", 1, 1, 1, 2, IN, OUT, sizeof(Transpose), false, construct_node<Transpose>,
    GROUPS, 1, IN_NAMES, OUT_NAMES,
    "Shifts every note up or down, by semitones and by octaves. A note that leaves 0..127 is dropped.",
    CATEGORY_MIDI };

// Clamped rather than refused, because this runs on a stored byte: a preset
// saved when the bounds were wider loads playing something close to what it
// said, where a refusal would lose the whole patch. A *runtime* write outside
// the bounds is a different thing and is rejected (set_param below), because
// there is a caller there to tell.
static int8_t clamp_to(uint8_t stored, int8_t limit){
    const int8_t value = param_centred(stored);
    if (value > limit) return limit;
    if (value < -limit) return -limit;
    return value;
}

Transpose::Transpose(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    semitones(clamp_to(config.params[0], MAX_SEMITONES)),
    octaves(clamp_to(config.params[1], MAX_OCTAVES)),
    sounding()
{}

void Transpose::process(BusManager& bus, uint32_t){
    const int16_t shift = offset();
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (is_note_off(e)){
            sounding.release(bus, out, e.data1);      // as sent, not as configured
            continue;
        }
        if (is_note_on(e)){
            const int16_t shifted = (int16_t)e.data1 + shift;
            if (shifted < 0 || shifted > 127) continue;   // dropped, off and all
            sounding.emit(bus, out, e.data1, (uint8_t)shifted, e.data2, e.channel);
            continue;
        }
        bus.note_write(out, e);
    }
}

bool Transpose::set_param(uint16_t index, uint8_t value){
    // The ledger releases at the sent pitch, so either control may move under
    // a held note.
    const int8_t wanted = param_centred(value);
    switch (index){
        case 0:
            if (wanted > MAX_SEMITONES || wanted < -MAX_SEMITONES) return false;
            semitones = wanted;
            return true;
        case 1:
            if (wanted > MAX_OCTAVES || wanted < -MAX_OCTAVES) return false;
            octaves = wanted;
            return true;
        default:
            return false;
    }
}

uint8_t Transpose::get_param(uint16_t index) const {
    switch (index){
        case 0:  return param_centred_byte(semitones);
        case 1:  return param_centred_byte(octaves);
        default: return 0;
    }
}

void Transpose::silence(BusManager& bus){
    sounding.release_all(bus, out);
}
