#ifndef MMMC_CLOCK_CLOCK_OUT_H
#define MMMC_CLOCK_CLOCK_OUT_H

#include <stdint.h>
#include "config.h"
#include "hal/imidi_out.h"

// The module's clock, out of its cables: MIDI clock at MASTER_PPQN and the
// transport's own start, stop and continue.
//
// **It is not a node, for the reason realtime never reaches a bus.** Clock
// and transport are transport-level in both directions (README, "MIDI"): a
// `MidiOutPort` writes channel-voice traffic taken off a note bus and would
// put a channel on anything it was handed, and a bus is published once a pass
// whereas the clock counts subticks. So the mask a clock leaves by is the
// clock's own setting, alongside the mask it listens on (MasterClock), and
// both travel in the patch's globals rather than in a node.
//
// **The count is what it is generated from**, not the incoming bytes. The
// module is a clock whatever moves it - its own timer, the sync jack, or a
// host - so a patch slaved to a DAW re-clocks the chain behind it from the
// same counter its own nodes run on, and CV sync into MIDI clock out is a
// patch nobody has to build.
class MidiClockOut {
    public:
        MidiClockOut();
        MidiClockOut(const MidiClockOut&) = delete;
        MidiClockOut& operator=(const MidiClockOut&) = delete;

        // The transport to send on. Once, from the master's constructor:
        // the cables move with the patch, the wire they are on does not.
        void attach(IMidiOut& transport);

        // The MidiPort bits the clock leaves by; 0 is nowhere, which is the
        // default. The control cable is masked off whatever is asked for:
        // twenty-four F8 bytes a quarter down the protocol's own cable would
        // be the patch taking the module away from its editor
        // (hal/midi_types.h).
        void set_target(uint8_t target_mask);
        uint8_t target_mask() const { return target; }

        // One pass, with the clock's subtick count, whether it is running and
        // the TransportEdge mask it latched. Transport first, then the clock
        // bytes the count has reached: a host that has just been started
        // hears the start before the downbeat it belongs to.
        void pass(uint32_t count, bool running, uint8_t edges);

        // Clock bytes one pass may send before it gives up and re-phases.
        // The count is collapsed per pass (master.h), so a pass that was held
        // off - a patch load, a flash write - comes back owing every subtick
        // of the gap. A quarter note of catch-up is a burst a downstream
        // sequencer can absorb; a second of it is a machine running away, and
        // an F8 that is late is better dropped than sent at the wrong time.
        static constexpr uint8_t MAX_BURST = MASTER_PPQN;

    private:
        void send(uint8_t type);
        // The first subtick at or after `count` that a clock byte is due on.
        static uint32_t next_boundary(uint32_t count);

        IMidiOut* out;
        uint8_t target;
        // The count the next clock byte is owed at. Only meaningful while
        // running; a start puts it back on the downbeat.
        uint32_t due;
        // False until `due` has been taken from a count this object has
        // actually seen. Switching the mask on under a running clock would
        // otherwise leave a whole count's worth of arrears behind it and
        // open with a burst; the first pass phases it instead.
        bool phased;
};

#endif
