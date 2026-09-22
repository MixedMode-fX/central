#ifndef MMMC_MONITOR_MONITOR_H
#define MMMC_MONITOR_MONITOR_H

#include <stdint.h>
#include "config.h"
#include "master.h"
#include "led/status_leds.h"
#include "monitor/hal_tap.h"

// The module, watched.
//
// With no display, the editor is the module's only picture of itself, and a
// picture has to be fed by what the instrument reports. This is the report:
// once a pass, after the signal path, it folds what a display needs into a
// fixed record - which gate buses and jacks have been high, the brightest
// each LED has been, where each CV bus is, and every note on or off that
// crossed a watched note bus or left on a cable, stamped with the pass that
// made it. An editor asks for the record over the protocol
// (SYSEX_MONITOR_REQUEST), gets everything since it last asked, and the fold
// starts again. An edge is therefore never missed by a display that reads
// slower than a pass; it can only be late.
//
// **It is armed by a request and disarms itself.** Nothing is folded, and
// nothing is sent, unless an editor has asked within MONITOR_ARMED_US: a
// module in a rack with no editor on the cable does one comparison a pass
// here and nothing else, and one whose editor went away is back to that a
// second later with nothing to tear down. A request is not a subscription.
//
// It is not a node and holds no bus index; it reads the buses' front buffer
// after the pass has published and writes nothing back, so the signal path
// cannot tell whether it is being watched. It reads the jacks and the sent
// notes off the tap on the hardware seam (hal_tap.h) rather than off the
// pins, so watching costs no pin access either.
//
// Where a node *is* - the step each lane of a sequencer is on, the chord a
// harmony is sounding and the loop it has written - is read on demand for
// the nodes a request names, because a playhead is drawn for the card that
// is open and not for every node in the patch.
class Monitor {
    public:
        // Where an event was seen: on a note bus (`arg` is the bus), or
        // leaving the module (`arg` is the cable mask).
        enum Where : uint8_t { WHERE_BUS = 0, WHERE_OUT = 1 };
        struct Event {
            uint32_t at_us;
            uint8_t where;
            uint8_t arg;
            uint8_t type;
            uint8_t channel;
            uint8_t d1;
            uint8_t d2;
        };
        // A position that is not one: a sequencer before its first advance,
        // a loop slot the walk has not reached.
        static constexpr uint8_t NONE = 0xFF;

        Monitor(const MixedModeMaster& master, HalTap& panel, const StatusLeds& status);
        Monitor(const Monitor&) = delete;
        Monitor& operator=(const Monitor&) = delete;

        // Once per pass, after the signal path and the LEDs. A no-op unless
        // armed, save for clearing the tap's sent notes.
        void sample(uint32_t now_us);

        // A request: arms the monitor, and names the note buses whose events
        // are worth keeping. A bus nobody watches is not read. A request to
        // a monitor that had gone cold starts from an empty record: what
        // happened while nobody was asking is not the answer to a new
        // question.
        void watch(uint16_t note_buses, uint32_t now_us);
        bool armed(uint32_t now_us) const {
            return watching && (uint32_t)(now_us - watched_at) < MONITOR_ARMED_US;
        }

        // The record since the last take(). `sampled_at` is the pass the
        // record ends on, the time every age in it is measured from.
        uint32_t sampled_at() const { return at; }
        uint32_t gate_now() const { return g_now; }
        uint32_t gate_since() const { return g_since; }
        uint16_t jack_in_now() const { return ji_now; }
        uint16_t jack_in_since() const { return ji_since; }
        uint16_t jack_out_now() const { return jo_now; }
        uint16_t jack_out_since() const { return jo_since; }
        uint8_t led_peak(uint8_t led) const { return led < LED_COUNT ? led_hi[led] : 0; }
        int16_t cv(uint8_t bus) const { return bus < N_CV_BUS ? cv_last[bus] : 0; }
        uint8_t event_count() const { return n_events; }
        const Event& event(uint8_t i) const { return events[i]; }
        // Events happened that the record had no room for.
        bool overflowed() const { return lost; }
        // The record has been read: the fold starts again from what is
        // high now.
        void take();

        // Where a node is, into `out`, and how many values that is. A
        // sequencer answers a step per lane; a harmony answers the degree
        // sounding, the slot of its loop that is sounding, and the degree in
        // each slot of the loop. NONE where there is nothing to point at, and
        // no values at all for a node that has no position.
        uint8_t positions(uint8_t node, uint8_t* out, uint8_t capacity) const;

    private:
        void record(uint32_t now_us, uint8_t where, uint8_t arg, const MidiEvent& e);
        void clear(uint32_t now_us);

        const MixedModeMaster& mm;
        HalTap& tap;
        const StatusLeds& leds;

        uint16_t note_mask;
        uint32_t watched_at;
        bool watching;

        uint32_t at;
        uint32_t g_now, g_since;
        uint16_t ji_now, ji_since, jo_now, jo_since;
        uint8_t led_hi[LED_COUNT];
        int16_t cv_last[N_CV_BUS];
        Event events[MONITOR_EVENTS];
        uint8_t n_events;
        bool lost;
};

#endif
