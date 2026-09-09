#ifndef MMMC_CLOCK_MASTER_CLOCK_H
#define MMMC_CLOCK_MASTER_CLOCK_H

#include <stdint.h>
#include "config.h"

// The one time base for the whole module (#4).
//
// The counter is in *subticks*: CLOCK_SUBTICK of them per PPQN tick, so a
// multiplier that divides CLOCK_SUBTICK is exact (see config.h). It only
// ever moves forward, so a node can compare two readings without worrying
// about the clock going backwards under it - except at start(), which is
// the one place the count is reset to zero, and which nodes detect by
// seeing a count lower than the one before.
//
// Three sources, all feeding the same counter:
//
//   Internal  a hardware timer at subtick_interval_us(), tapped or set in BPM.
//   CV        rising edges on the sync jack, at cv_ppqn() pulses per quarter.
//   MIDI      MIDI clock (0xF8) bytes, at MASTER_PPQN per quarter, with
//             start / stop / continue driving the transport state.
//
// For the two external sources the timer keeps running between edges at the
// interval the last measured edge period implies, and each edge re-phases the
// counter onto the subtick boundary it belongs on. The timer therefore fills
// in the subticks an external source does not deliver, and the edge - not the
// free-running estimate - is what the count is anchored to.
//
// Threading: advance() and external_edge() are called from interrupt context
// (the interval timer and the sync pin), everything else from the main loop.
// On the Teensy both interrupts run at the same priority, so neither can
// preempt the other, and a 32-bit aligned load or store is atomic on the
// Cortex-M7. consume() is the only reader of the counter that matters, and it
// reads it once.
class MasterClock {
    public:
        enum Source : uint8_t {
            CLOCK_INTERNAL = 0,
            CLOCK_CV       = 1,
            CLOCK_MIDI     = 2,
        };

        MasterClock();

        // Configuration ---------------------------------------------------
        void set_source(uint8_t source);
        uint8_t source() const { return src; }
        // Clamped to CLOCK_MIN_BPM .. CLOCK_MAX_BPM.
        void set_bpm(uint16_t beats_per_minute);
        // Tap tempo (#21): the interval between two taps is one beat. A gap
        // longer than TAP_TIMEOUT_US starts a new measurement rather than
        // averaging across a pause, and a tap implying a tempo outside the
        // limits is ignored rather than clamped - a stray tap should not
        // silently pin the tempo to 20 BPM. Averaged over the last few taps,
        // because two taps by hand are not an accurate beat.
        void tap(uint32_t now_us);
        static constexpr uint32_t TAP_TIMEOUT_US = 3000000;
        static constexpr uint8_t TAP_AVERAGE = 4;
        uint16_t bpm() const { return tempo; }
        // Pulses per quarter note expected at the sync jack (1, 2, 4, 24, ...).
        void set_cv_ppqn(uint8_t ppqn);
        uint8_t cv_ppqn() const { return cv_pulses; }

        // Transport --------------------------------------------------------
        // Resets the count to zero and runs: the downbeat is subtick 0.
        void start();
        // Stops the counter; the count is kept so continue() resumes on it.
        void stop();
        void resume();
        bool running() const { return is_running; }

        // Timer ------------------------------------------------------------
        // What the interval timer should be programmed at, in microseconds.
        uint32_t subtick_interval_us() const { return interval_us; }
        // True once after the interval changed; clears the flag. The main
        // loop reprograms the timer, so no timer API is called from an ISR.
        bool take_interval_change();

        // Interrupt-context entry points -----------------------------------
        // One subtick. Ignored while stopped.
        void advance();
        // One edge of the selected external source, at `now_us`.
        void external_edge(uint32_t now_us);

        // MIDI realtime, routed here rather than onto a note bus (#5).
        // Clock bytes only advance the counter while the MIDI source is
        // selected; start / stop / continue always apply, so a patch can be
        // armed from a DAW before the source is switched.
        void midi_message(uint8_t type, uint32_t now_us);

        // Main loop ----------------------------------------------------------
        // True when the count changed since the last call, with the newest
        // count in `count_out`. Subticks that arrived between two calls are
        // collapsed into one report: nodes work from the count, not from the
        // number of calls, so nothing is lost.
        bool consume(uint32_t& count_out);
        uint32_t count() const { return subticks; }

        // Diagnostics --------------------------------------------------------
        // Edges the external source delivered that the estimator rejected as
        // implausible (a period outside MIN_PERIOD_US .. MAX_PERIOD_US).
        uint32_t rejected_edges() const { return rejected; }

        // Plausible interval between two external edges. Below the low bound
        // is contact noise or a jitter burst; above it the source has stopped
        // rather than slowed, and the estimate is not worth keeping.
        static constexpr uint32_t MIN_PERIOD_US = 200;
        static constexpr uint32_t MAX_PERIOD_US = 3000000;

    private:
        uint32_t subticks_per_edge() const;
        void recompute_internal_interval();
        void set_interval(uint32_t us);

        volatile uint32_t subticks;
        volatile uint32_t interval_us;
        volatile uint32_t edge_index;
        volatile uint32_t last_edge_us;
        volatile uint32_t rejected;
        volatile bool interval_dirty;
        volatile bool is_running;
        volatile bool have_edge;
        uint32_t last_consumed;
        uint32_t last_tap_us;
        uint32_t tap_intervals[TAP_AVERAGE];
        uint8_t tap_count;
        uint16_t tempo;
        uint8_t src;
        uint8_t cv_pulses;
};

#endif
