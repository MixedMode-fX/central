#include "algorithm/midi/note_delay.h"
#include "node/registry.h"
#include "midi/note_event.h"
#include "midi/global_scale.h"
#include "hal/midi_types.h"

static const Domain IN[2] = {Domain::Note, Domain::Gate};
static const Domain OUT[1] = {Domain::Note};

static const char* const SYNC_NAMES[NoteDelay::ND_SYNCS] = {"clock", "free"};
static const char* const DRY_NAMES[NoteDelay::ND_DRYS] = {"pass", "mute"};

static const ParamDescriptor PARAMS[12] = {
    {"sync",     NoteDelay::ND_CLOCK, NoteDelay::ND_SYNCS, NoteDelay::ND_CLOCK, PARAM_ENUM, SYNC_NAMES},
    {"division", DIV_8_BARS,    DIVISIONS, DIV_EIGHTH,    PARAM_ENUM, DIVISION_NAMES},
    {"feel",     FEEL_STRAIGHT, FEELS,     FEEL_STRAIGHT, PARAM_ENUM, FEEL_NAMES},
    {"time",     1, 255, 25, PARAM_NUMBER, nullptr},          // tens of milliseconds
    {"repeats",  1, NoteDelay::MAX_REPEATS, 3, PARAM_NUMBER, nullptr},
    {"interval", 0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"decay",    0, 100, 70,  PARAM_PERCENT, nullptr},
    {"chance",   0, 100, 100, PARAM_PERCENT, nullptr},
    {"spread",   0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"scale",    0, SCALE_COUNT - 1, 0, PARAM_ENUM, PARAM_SCALE_NAMES},
    {"channel",  0, 16, 0, PARAM_CHANNEL, nullptr},
    {"dry",      NoteDelay::ND_PASS, NoteDelay::ND_DRYS, NoteDelay::ND_PASS, PARAM_ENUM, DRY_NAMES},
};
static const ParamGroup GROUPS[1] = {{0, 1, 12, PARAMS}};

static const char* const IN_NAMES[2] = {"notes in", "clear"};
static const char* const OUT_NAMES[1] = {"notes out"};

const AlgorithmDescriptor NoteDelay::descriptor = {
    ALGO_NOTE_DELAY, "NoteDelay", 2, 1, 1, 12, IN, OUT, sizeof(NoteDelay), true,
    construct_node<NoteDelay>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A delay that is a canon: repeats transposed in the key, and spread off the grid.",
    CATEGORY_MIDI };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

static int8_t as_signed(uint8_t stored){ return (int8_t)stored; }

NoteDelay::NoteDelay(const NodeConfig& config) :
    in(config.in_bus[0]),
    out(config.out_bus[0]),
    sync(clamp_enum(config.params[0], ND_SYNCS, ND_CLOCK)),
    div(clamp_enum(config.params[1], DIVISIONS, DIV_EIGHTH)),
    how(clamp_enum(config.params[2], FEELS, FEEL_STRAIGHT)),
    time_param(config.params[3] ? config.params[3] : (uint8_t)25),
    repeats(config.params[4] ? (config.params[4] > MAX_REPEATS ? MAX_REPEATS : config.params[4]) : (uint8_t)3),
    interval(config.params[5]),
    decay(config.params[6] ? (config.params[6] > 100 ? (uint8_t)100 : config.params[6]) : (uint8_t)70),
    chance(step_probability(config.params[7])),
    spread(config.params[8]),
    scale(config.params[9] < SCALE_COUNT ? config.params[9] : (uint8_t)0),
    channel(config.params[10] > 16 ? (uint8_t)0 : config.params[10]),
    dry(clamp_enum(config.params[11], ND_DRYS, ND_PASS)),
    subtick(0), drops(0),
    clear_in(config.in_bus[1]),
    rng(entropy::seed()),
    echoes()
{
    for (uint8_t i = 0; i < MAX_ECHOES; i++) echoes[i].state = ECHO_FREE;
}

uint8_t NoteDelay::scheduled() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_ECHOES; i++) if (echoes[i].state == ECHO_WAITING) n++;
    return n;
}

uint8_t NoteDelay::sounding_count() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_ECHOES; i++) if (echoes[i].state == ECHO_SOUNDING) n++;
    return n;
}

uint32_t NoteDelay::unit_delay() const {
    if (sync == ND_CLOCK) return division_subticks(div, how);
    return (uint32_t)time_param * 10000u;                    // tens of ms, in microseconds
}

// How far repeat k sits from the note that caused it.
//
// With `spread` at zero this is k periods and nothing else: a plain delay.
// Otherwise each gap is `spread` percent longer than the one before it, so
// the total grows with the square of k - which is what makes the repeats
// walk off the grid instead of sitting on a slower one. A negative spread
// shortens each gap in turn and the echoes accelerate; the total is floored
// at one unit of the clock so a repeat can never arrive before, or with, the
// note that caused it.
uint32_t NoteDelay::delay_of(uint8_t k) const {
    if (k == 0) return 0;
    const uint32_t period = unit_delay();
    int32_t s = as_signed(spread);
    if (s > 100) s = 100;
    if (s < -100) s = -100;
    // k periods, plus spread% of one period for each gap after the first:
    // 1 + 2 + ... + (k-1) of them.
    const int32_t triangle = (int32_t)k * ((int32_t)k - 1) / 2;
    int64_t total = (int64_t)period * (int64_t)k
                  + ((int64_t)period * s * triangle) / 100;
    if (total < 1) total = 1;
    return (uint32_t)total;
}

// The pitch of repeat k, transposed by k intervals **of the scale**.
//
// At interval 0 the pitch is passed through untouched rather than being
// round-tripped through the scale, so a delay that is not transposing is
// transparent to a chromatic line. When it *is* transposing, a source pitch
// the scale does not contain resolves into the scale first
// (semitone_to_scale_degree is total, which is what makes that safe), so a
// passing note's echoes land in the key even though the note did not.
uint8_t NoteDelay::pitch_for(uint8_t pitch, uint8_t k) const {
    const int8_t step = as_signed(interval);
    if (step == 0 || k == 0) return pitch;
    const uint16_t mask = global_scale::resolve_id(scale);
    const uint8_t r = global_scale::resolve_root(scale, 0);
    const int16_t rel = (int16_t)pitch - (int16_t)r;
    const int16_t degree = semitone_to_scale_degree(rel, mask);
    const int16_t moved = (int16_t)(degree + (int16_t)step * (int16_t)k);
    const int16_t result = (int16_t)r + scale_degree_to_semitone(moved, mask);
    if (result < 0 || result > 127) return 0xFF;
    return (uint8_t)result;
}

void NoteDelay::schedule(uint8_t source, uint8_t velocity, uint8_t ch, uint32_t now_us){
    const bool by_subtick = (sync == ND_CLOCK);
    const uint32_t origin = by_subtick ? subtick : now_us;
    uint32_t level = velocity;

    for (uint8_t k = 1; k <= repeats; k++){
        level = (level * decay) / 100u;
        if (level == 0) break;                 // quieter than a note-off: stop here
        if (!rng.chance(chance)) continue;     // each repeat rolls for itself

        const uint8_t pitch = pitch_for(source, k);
        if (pitch == 0xFF) continue;           // off the end of the keyboard: a rest

        uint8_t slot = MAX_ECHOES;
        for (uint8_t i = 0; i < MAX_ECHOES; i++){
            if (echoes[i].state == ECHO_FREE){ slot = i; break; }
        }
        if (slot == MAX_ECHOES){
            // Nothing that cannot be released is emitted, which is the only
            // policy that keeps "no hanging notes" true under overflow.
            drops++;
            continue;
        }

        Echo& e = echoes[slot];
        e.on_at = origin + delay_of(k);
        e.off_at = 0;
        e.pitch = pitch;
        e.velocity = (uint8_t)(level > 127 ? 127 : level);
        e.channel = ch;
        e.source = source;
        e.repeat = k;
        e.state = ECHO_WAITING;
        e.by_subtick = by_subtick;
        e.have_off = false;
    }
}

// The source has been released, so its echoes are released the same distance
// later - which is what makes the repeat as long as the note was held.
void NoteDelay::note_off_arrived(uint8_t source, uint32_t now_us){
    for (uint8_t i = 0; i < MAX_ECHOES; i++){
        Echo& e = echoes[i];
        if (e.state == ECHO_FREE || e.have_off || e.source != source) continue;
        const uint32_t origin = e.by_subtick ? subtick : now_us;
        e.off_at = origin + delay_of(e.repeat);
        e.have_off = true;
    }
}

bool NoteDelay::due(const Echo& e, uint32_t at, uint32_t now_us) const {
    const uint32_t clock = e.by_subtick ? subtick : now_us;
    // Unsigned, so a wrap of either counter is a moment early rather than
    // seventy-one minutes late.
    return (uint32_t)(clock - at) < 0x80000000u;
}

void NoteDelay::release(BusManager& bus, Echo& e){
    // Released with the pitch and channel it was *sent* with, taken from the
    // record rather than recomputed: the scale, the interval and the key may
    // all have moved since.
    const MidiEvent off = {MIDI_NOTE_OFF, e.channel, e.pitch, 0};
    bus.note_write(out, off);
    e.state = ECHO_FREE;
}

void NoteDelay::tick(BusManager&, uint32_t count){ subtick = count; }

void NoteDelay::process(BusManager& bus, uint32_t now_us){
    if (clear_in.rising(bus)){
        for (uint8_t i = 0; i < MAX_ECHOES; i++){
            if (echoes[i].state == ECHO_SOUNDING) release(bus, echoes[i]);
            echoes[i].state = ECHO_FREE;
        }
    }

    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++){
        const MidiEvent e = bus.note_read(in, i);
        if (dry == ND_PASS) bus.note_write(out, e);
        if (is_note_off(e)){ note_off_arrived(e.data1, now_us); continue; }
        if (!is_note_on(e)) continue;          // CC, bend and the rest are not echoed
        schedule(e.data1, e.data2, channel ? channel : e.channel, now_us);
    }

    for (uint8_t i = 0; i < MAX_ECHOES; i++){
        Echo& e = echoes[i];
        if (e.state == ECHO_WAITING && due(e, e.on_at, now_us)){
            const MidiEvent on = {MIDI_NOTE_ON, e.channel, e.pitch, e.velocity};
            bus.note_write(out, on);
            e.state = ECHO_SOUNDING;
        }
        if (e.state == ECHO_SOUNDING && e.have_off && due(e, e.off_at, now_us)){
            release(bus, e);
        }
    }
}

void NoteDelay::silence(BusManager& bus){
    for (uint8_t i = 0; i < MAX_ECHOES; i++){
        if (echoes[i].state == ECHO_SOUNDING) release(bus, echoes[i]);
        echoes[i].state = ECHO_FREE;
    }
}

bool NoteDelay::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: if (value == 0 || value > ND_SYNCS) return false; sync = value; return true;
        case 1: if (value == 0 || value > DIVISIONS) return false; div = value; return true;
        case 2: if (value == 0 || value > FEELS) return false; how = value; return true;
        case 3: if (value == 0) return false; time_param = value; return true;
        case 4: if (value == 0 || value > MAX_REPEATS) return false; repeats = value; return true;
        case 5: interval = value; return true;
        case 6: if (value > 100) return false; decay = value; return true;
        case 7: if (value > 100) return false; chance = step_probability(value); return true;
        case 8: spread = value; return true;
        case 9: if (value >= SCALE_COUNT) return false; scale = value; return true;
        case 10: if (value > 16) return false; channel = value; return true;
        case 11: if (value == 0 || value > ND_DRYS) return false; dry = value; return true;
        default: return false;
    }
}

uint8_t NoteDelay::get_param(uint16_t index) const {
    switch (index){
        case 0: return sync;
        case 1: return div;
        case 2: return how;
        case 3: return time_param;
        case 4: return repeats;
        case 5: return interval;
        case 6: return decay;
        case 7: return chance;
        case 8: return spread;
        case 9: return scale;
        case 10: return channel;
        case 11: return dry;
        default: return 0;
    }
}
