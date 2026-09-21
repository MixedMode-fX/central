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
    PARAM_CHANNEL,      // MIDI channel, 1..16 (0, where a descriptor allows it, is omni)
    PARAM_CENTRED,      // a byte biased by PARAM_CENTRE: the value is stored - 128
    PARAM_CHANNEL_OUT,  // the channel a node sends on; 0 keeps the incoming one
    PARAM_ENV_TIME,     // an envelope stage's length: see ENV_TIME_UNIT_US
};

// **A channel a node *reads* and a channel a node *sends on* are not the same
// control, and zero is where they part.** Both are a number from 1 to 16 with
// a spare zero, so one kind would render both - and would have to pick one
// word for that zero. On an input it means omni, every channel at once; on an
// output there is no such thing, and it means the message leaves on the
// channel it arrived on. An editor showing "omni" over a transposer's output
// is telling the player the opposite of what the byte does, so the two kinds
// are separate and each says its own zero.

// What one step of a PARAM_ENV_TIME byte is worth, at the bottom of its range.
//
// **An envelope stage cannot be a linear byte of milliseconds.** The stages a
// musician wants run from an instant attack to a thirty-second swell, and one
// byte cannot hold both linearly: at 1 ms a step the range stops at a quarter
// of a second, and at 128 ms a step the shortest attack there is is an eighth
// of a second, which is a pad and never a pluck. So the byte is squared -
// `n` is `n * n * ENV_TIME_UNIT_US` microseconds - which spends its
// resolution where the ear is: half a millisecond a step at the fast end,
// where the difference between 5 ms and 10 ms is the difference between a
// click and a knock, and a quarter of a second a step at the slow end, where
// it is under one percent of a thirty-second fade.
//
// 1 is half a millisecond, which is one pass of the graph and is the byte
// that means "no stage at all"; 255 is 32.5 seconds.
//
// A #define, and the arithmetic is one multiply, because app/src/protocol/generated.js
// is generated from these headers and the editor has to print the same
// seconds the firmware counts (app/tools/generate-protocol.mjs).
#define ENV_TIME_UNIT_US 500

// What a PARAM_ENV_TIME byte is worth, in microseconds.
inline uint32_t param_env_time_us(uint8_t stored){
    return (uint32_t)stored * (uint32_t)stored * (uint32_t)ENV_TIME_UNIT_US;
}

// The zero of a PARAM_CENTRED byte.
//
// **A bipolar control with real bounds cannot live in a PARAM_SIGNED byte.**
// Everything that sweeps a parameter from outside - a mapped CC, a CV route, an
// NRPN - scales and clamps inside one rising interval [min, max], and -12..+12
// as an int8 is not one: it is 244..255 followed by 0..12. A knob sweeping
// that would jump from the top of the range to the bottom halfway up its
// travel, and `min > max` breaks the clamp outright. So a parameter that is
// bipolar *and* bounded stores an unsigned byte biased by 128, min and max are
// the plain byte bounds they are everywhere else, and nothing that sweeps a
// parameter has to know the difference. PARAM_SIGNED stays what it is: the
// unbounded int8, for the parameters that take the whole -128..+127.
//
// The bias is a constant and not the middle of the range, so widening or
// narrowing a bounded control later does not re-read every byte already saved
// in a patch: +12 semitones is 140 whether the control stops at 12 or at 24.
//
// A #define rather than a constant, because app/src/protocol.js is generated
// from these headers and the editor has to read the same zero the firmware
// writes (app/tools/generate-protocol.mjs).
#define PARAM_CENTRE 128

// What a PARAM_CENTRED byte means. Zero means the default everywhere, and a
// centred parameter's default is the centre, so an untouched byte is no shift
// rather than -128.
inline int8_t param_centred(uint8_t stored){
    // Not named `byte`: the Teensy core typedefs that, and -Wshadow is an error.
    const int16_t raw = (stored == 0) ? (int16_t)PARAM_CENTRE : (int16_t)stored;
    return (int8_t)(raw - (int16_t)PARAM_CENTRE);
}

// The byte that means `value`, for a PARAM_CENTRED parameter.
inline uint8_t param_centred_byte(int8_t value){
    return (uint8_t)((int16_t)value + (int16_t)PARAM_CENTRE);
}

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
