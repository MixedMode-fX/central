#include "clock/master_clock.h"
#include "hal/midi_types.h"

MasterClock::MasterClock() :
    subticks(0), interval_us(0), edge_index(0), last_edge_us(0), rejected(0),
    interval_dirty(true), is_running(true), have_edge(false),
    last_consumed(0), last_tap_us(0), tap_intervals(), tap_count(0),
    tempo(CLOCK_DEFAULT_BPM),
    src(CLOCK_INTERNAL), cv_pulses(4)
{
    recompute_internal_interval();
}

// Subticks one edge of the selected external source stands for. A MIDI clock
// byte is one PPQN tick; a sync pulse is a quarter divided by cv_ppqn.
uint32_t MasterClock::subticks_per_edge() const {
    if (src == CLOCK_MIDI) return CLOCK_SUBTICK;
    const uint8_t p = cv_pulses ? cv_pulses : 1;
    const uint32_t n = (uint32_t)CLOCK_SUBTICKS_PER_QUARTER / p;
    return n ? n : 1;
}

void MasterClock::set_interval(uint32_t us){
    if (us == 0) us = 1;
    if (us == interval_us) return;
    interval_us = us;
    interval_dirty = true;
}

void MasterClock::recompute_internal_interval(){
    // 60 s per minute / (BPM * subticks per quarter). The truncation costs
    // about 0.01% of the tempo at 120 BPM, which is a fixed offset rather
    // than a drift: every subtick is the same length, so two dividers stay
    // locked to each other whatever the rounding does to absolute tempo.
    const uint32_t divisor = (uint32_t)tempo * (uint32_t)CLOCK_SUBTICKS_PER_QUARTER;
    set_interval(60000000UL / divisor);
}

void MasterClock::set_source(uint8_t source){
    if (source > CLOCK_MIDI) return;
    if (source == src) return;
    src = source;
    have_edge = false;
    last_edge_us = 0;
    edge_index = subticks / subticks_per_edge();
    // Until an external source delivers its first two edges there is no
    // period to measure, so the timer free-runs at the internal tempo.
    if (src == CLOCK_INTERNAL) recompute_internal_interval();
}

void MasterClock::set_bpm(uint16_t beats_per_minute){
    uint16_t b = beats_per_minute;
    if (b < CLOCK_MIN_BPM) b = CLOCK_MIN_BPM;
    if (b > CLOCK_MAX_BPM) b = CLOCK_MAX_BPM;
    tempo = b;
    if (src == CLOCK_INTERNAL) recompute_internal_interval();
}

void MasterClock::set_cv_ppqn(uint8_t ppqn){
    if (ppqn == 0) return;
    cv_pulses = ppqn;
    have_edge = false;
}

void MasterClock::start(){
    subticks = 0;
    edge_index = 0;
    last_edge_us = 0;
    have_edge = false;
    is_running = true;
}

void MasterClock::stop(){
    is_running = false;
}

void MasterClock::resume(){
    have_edge = false;
    is_running = true;
}

bool MasterClock::take_interval_change(){
    if (!interval_dirty) return false;
    interval_dirty = false;
    return true;
}

void MasterClock::advance(){
    if (!is_running) return;
    subticks = subticks + 1;
}

void MasterClock::external_edge(uint32_t now_us){
    if (!is_running) return;

    if (have_edge){
        const uint32_t period = now_us - last_edge_us;   // wrap-safe
        if (period >= MIN_PERIOD_US && period <= MAX_PERIOD_US){
            set_interval(period / subticks_per_edge());
        } else {
            rejected = rejected + 1;
        }
    }
    last_edge_us = now_us;
    have_edge = true;

    // Re-phase: this edge is the start of subtick edge_index * n. Catching up
    // moves the counter forward, never back, so a node's tick arithmetic
    // never sees time reverse; a timer that ran ahead of the source is
    // absorbed by re-deriving the edge index from the count instead.
    const uint32_t n = subticks_per_edge();
    edge_index = edge_index + 1;
    const uint32_t target = edge_index * n;
    if (subticks < target) subticks = target;
    else                   edge_index = subticks / n;
}

void MasterClock::midi_message(uint8_t type, uint32_t now_us){
    switch (type){
        case MIDI_CLOCK:
            if (src == CLOCK_MIDI) external_edge(now_us);
            break;
        case MIDI_START:    start();  break;
        case MIDI_CONTINUE: resume(); break;
        case MIDI_STOP:     stop();   break;
        default: break;
    }
}

bool MasterClock::consume(uint32_t& count_out){
    const uint32_t now = subticks;
    if (now == last_consumed) return false;
    last_consumed = now;
    count_out = now;
    return true;
}

// Tap tempo. The header comment has promised "tapped or set in BPM" since #4;
// this is the entry point, reached from a CC bound to CC_TRANSPORT_TAP (#21)
// or from the console.
void MasterClock::tap(uint32_t now_us){
    const uint32_t gap = (uint32_t)(now_us - last_tap_us);
    last_tap_us = now_us;

    // A long gap is a new tempo, not a slow one: averaging across a pause
    // would make the first tap after a break drag everything down.
    if (tap_count == 0 || gap > TAP_TIMEOUT_US){
        tap_count = 1;                 // this tap starts the measurement
        return;
    }

    // Keep the last few intervals and average them: two taps by hand are not
    // an accurate beat, four are much closer.
    for (uint8_t i = TAP_AVERAGE - 1u; i > 0; i--) tap_intervals[i] = tap_intervals[i - 1u];
    tap_intervals[0] = gap;
    if (tap_count < TAP_AVERAGE + 1u) tap_count++;

    const uint8_t n = (uint8_t)(tap_count - 1u);
    uint32_t total = 0;
    for (uint8_t i = 0; i < n && i < TAP_AVERAGE; i++) total += tap_intervals[i];
    const uint32_t mean = total / (n < TAP_AVERAGE ? n : TAP_AVERAGE);
    if (mean == 0) return;

    const uint32_t bpm = 60000000u / mean;
    // Outside the limits the tap was a mistake - a double-hit, or the first
    // of a new phrase - and pinning the tempo to a limit would be worse than
    // ignoring it.
    if (bpm < CLOCK_MIN_BPM || bpm > CLOCK_MAX_BPM) return;
    set_bpm((uint16_t)bpm);
}
