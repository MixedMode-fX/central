#ifndef MMMC_NODE_PARAM_H
#define MMMC_NODE_PARAM_H

#include <stdint.h>

// What a parameter means, so a host can render it (#20).
//
// AlgorithmDescriptor used to carry n_params and nothing else, which is not
// enough for anybody: the editor (#12) cannot draw a control for a range it
// does not know, CC mapping (#21) cannot scale 0..127 onto it, and the
// validator cannot range-check it. The kind is what lets an editor show a
// scale as a list and a trigger width as a millisecond field without a
// hardcoded table per algorithm that drifts the moment one is added.
enum ParamKind : uint8_t {
    PARAM_NUMBER = 0,   // a plain count
    PARAM_ENUM,         // one of `options`, indexed from min
    PARAM_BOOL,         // zero or non-zero
    PARAM_BITFIELD,     // eight steps of a pattern, step n in bit n
    PARAM_PITCH,        // MIDI note, 0..127
    PARAM_PITCH_CLASS,  // 0..11
    PARAM_SIGNED,       // an int8 stored in the byte: 128..255 are -128..-1
    PARAM_MILLIS,
    PARAM_PERCENT,
    PARAM_CHANNEL,      // MIDI channel, 1..16
};

// One parameter's range and meaning.
//
// `def` is what the byte behaves as when it is left at zero. Several
// algorithms use 0 as "use my default" - a gate sequencer's length, a
// divider's amount, a trigger width - which is right for a zero-initialised
// preset and poison for a knob, so the rule is uniform across every
// algorithm and stated once here:
//
//   * A stored 0 is always accepted by the validator and means `def`. That
//     substitution happens **at construction only**, where an all-zero
//     NodeConfig still has to mean something sensible.
//   * `min` and `max` are the real bounds. A runtime write outside them is
//     rejected by MixedModeMaster::set_node_param, which is why a knob
//     sweeping length down through 1 cannot land on 0 and jump to 8: #21
//     scales a controller onto [min, max] and never produces 0 for a
//     parameter whose min is 1.
struct ParamDescriptor {
    const char* name;
    uint8_t     min;
    uint8_t     max;
    uint8_t     def;
    uint8_t     kind;               // ParamKind
    const char* const* options;     // (max - min + 1) names when kind is PARAM_ENUM
};

// A run of parameters that share one shape.
//
// `repeat` groups of `n_fields` descriptors, starting at parameter `first`:
// repeat == 1 describes a plain header block, repeat > 1 a table whose fields
// recur with a stride of n_fields - a sequencer's steps, a drum machine's
// lanes. A PolySequencer's 336 parameters are two groups and 26 descriptors
// this way, rather than 336 hand-written entries that would drift the moment
// a voice is added.
//
// **`label` is what the group is, when the algorithm knows better than the
// editor does.** An editor with no other information sorts parameters by
// what their names sound like, which is right often enough to be worth
// doing and wrong exactly where an algorithm has several controls over one
// mechanism: Harmony's `spread` shapes a chord walk and NoteDelay's spreads
// echoes in time, and no table keyed on the word can put both in the right
// place. An algorithm that says so splits its parameters into labelled
// groups and the editor uses them; one that says nothing keeps a single
// unlabelled group and the editor keeps guessing, which is what every
// algorithm here did before and most still do.
//
// Defaulted rather than positional so that saying nothing costs nothing: the
// thirty-odd descriptors that have no opinion are unchanged.
struct ParamGroup {
    uint16_t first;
    uint16_t repeat;
    uint16_t n_fields;
    const ParamDescriptor* fields;
    const char* label = nullptr;
};

// Cost, measured rather than guessed (#20 asks for the number): describing
// all 25 algorithms adds **4096 bytes of flash data** and 2656 bytes of code
// for the set_param / get_param implementations - 6.7 KB against the Teensy
// 4.1's 8 MB. The names are the largest part and stay unconditional; if that
// ever becomes uncomfortable it is the names that go behind a build flag,
// never the ranges, because the ranges are what the validator enforces.

// The descriptor for one parameter index, or nullptr if no group covers it.
inline const ParamDescriptor* param_lookup(const ParamGroup* groups, uint8_t n_groups, uint16_t index){
    for (uint8_t g = 0; g < n_groups; g++){
        const ParamGroup& grp = groups[g];
        const uint32_t span = (uint32_t)grp.repeat * grp.n_fields;
        if (index < grp.first || index >= grp.first + span) continue;
        return &grp.fields[(index - grp.first) % grp.n_fields];
    }
    return nullptr;
}

// The value a stored byte behaves as: zero means the descriptor's default.
inline uint8_t param_effective(const ParamDescriptor& d, uint8_t stored){
    return stored == 0 ? d.def : stored;
}

// Shared option-name tables, so two algorithms naming the same enum agree.
extern const char* const PARAM_DIRECTION_NAMES[5];   // StepEngine::Direction
extern const char* const PARAM_SCALE_NAMES[15];      // ScaleId
// The register a node plays in. Index 0 is not an octave but the key's own
// (midi/global_key.h): a node left alone moves with the key, and one that
// names an octave stays in it whatever the key does. Octave 0 would be MIDI
// notes 0..11, which no patch wants, so nothing is given up by spending it
// on the default.
extern const char* const PARAM_OCTAVE_NAMES[11];
// NotePriorityRule (midi/held_notes.h). Options are indexed from the
// parameter's own minimum, so the one table serves a parameter numbered from
// zero and one numbered from one.
extern const char* const PARAM_PRIORITY_NAMES[3];

#endif
