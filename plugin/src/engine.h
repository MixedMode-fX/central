#ifndef MMMC_PLUGIN_ENGINE_H
#define MMMC_PLUGIN_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "master.h"
#include "midi/midi_queue.h"
#include "led/status_leds.h"
#include "patch/patch_store.h"
#include "patch/patch_manager.h"
#include "protocol/sysex_handler.h"
#include "monitor/hal_tap.h"
#include "monitor/monitor.h"
#include "control/macros.h"
#include "control/cc_mapper.h"
#include "control/control_sum.h"
#include "control/mod_matrix.h"
#include "control/nrpn.h"
#include "plugin_hal.h"

// The module inside a plugin, with no plugin framework in sight.
//
// This is main.cpp's loop over the plugin's HAL, the way emulator/src/
// emu_api.cpp is main.cpp's loop over the page's: one MixedModeMaster, the
// control plane wired exactly as the firmware wires it, and the passes run
// from a clock the host owns. Nothing here includes JUCE, so it is built and
// tested on any host compiler by plugin/test/engine_test.cpp; the two files
// that know what a DAW is are processor.cpp and editor.cpp.
//
// **Time is the sample clock.** A pass is PASS_US of it, as it is a
// millisecond of wall clock in the page, and a block of samples is however
// many passes fall inside it. Every message from the host carries the sample
// it belongs at and is heard by the pass that covers it; every message the
// module sends is stamped with the sample of the pass that made it. The
// module is therefore sample-accurate to within a pass, and a block that is
// shorter than a pass simply carries no pass and the next one does.
//
// **The host is a MIDI clock.** A module in a rack follows the DAW by MIDI
// clock, and this one does too: HostClock turns the playhead into the
// realtime bytes the firmware already understands, on the host's own input
// cable, so a patch whose clock source is MIDI follows the transport and a
// patch on the internal clock runs free - the firmware's rule, not a new one.
// The plugin's own default is MIDI, because a plugin that ignored the play
// button would be the odd one out on the track.

namespace mmmc_plugin {

// The cable the host's MIDI input arrives on. The plugin's own window plays
// on whichever cable the page chooses, which is how a bound controller on
// the surface and a part on the DAW track stay two inputs of the patch.
constexpr uint8_t HOST_IN = mmMIDI_USB_0;

// Simulated microseconds per pass - the page's PASS_US.
constexpr uint32_t PASS_US = 1000;

// One message into the module, at a sample of the block it arrived in.
// Realtime is a type of 0xF8 and up with the rest zero.
struct InEvent {
    uint32_t sample;
    uint8_t  source;
    uint8_t  type;
    uint8_t  channel;
    uint8_t  d1;
    uint8_t  d2;
};

// The host's transport, as the MIDI realtime a slaved module hears.
//
// **Start is the downbeat and every clock byte is the end of a tick.** A
// MIDI clock byte moves the module's count to the next multiple of a tick
// (clock/master_clock.h), so the one at the downbeat itself is never sent:
// START puts the count on zero, and the first F8 is the first boundary
// *after* it. Pulses are on the host's absolute grid, every 1/MASTER_PPQN of
// a quarter, so a beat of the module lands on a beat of the DAW; where the
// bar falls is where play was pressed, which is what MIDI clock has always
// meant and why a rack starts its slaves from bar one. A loop or a relocate
// re-phases the pulses without a start: the count keeps going forward, as
// it does from a jack.
class HostClock {
    public:
        HostClock();

        // One block: `n_samples` from `ppq` at `bpm`, playing or not.
        // Appends the realtime the block owes to `out`, in sample order, and
        // returns how many. A bpm of zero means the host said nothing about
        // tempo, and a playing host with no tempo gets a transport and no
        // pulses.
        uint32_t block(bool playing, double ppq, double bpm, double sample_rate,
                       uint32_t n_samples, InEvent* out, uint32_t capacity);
        // Forget the transport: the next playing block sends a start.
        void reset();

    private:
        static InEvent realtime(uint32_t sample, uint8_t type);
        bool   was_playing;
        double expected_ppq;   // where the last block ended
        int64_t next_pulse;    // index of the next clock byte, in 1/MASTER_PPQN quarters
};

class Engine {
    public:
        Engine();
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Once, before anything else: stirs the entropy pool as main.cpp
        // does before any node is constructed, and boots as the module
        // boots - slot 0 if it checks out, the empty default if not, with
        // the clock following the host.
        void boot(uint32_t seed_a, uint32_t seed_b);

        // The host's sample rate, before the first block and whenever it
        // changes. The next pass is at the start of the next block.
        void set_sample_rate(double samples_per_second);

        // One block of `n_samples`, with the messages it carries in sample
        // order. Runs every pass the block covers; what the module sent is
        // in midi() afterwards, stamped with block samples, until the next
        // block clears it.
        void process(uint32_t n_samples, const InEvent* in, uint32_t n_in);

        // A message from the plugin's own window, on the cable it names.
        // Queued for the next pass, as a transport enqueues.
        void receive(uint8_t source, uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2);
        // One complete SysEx message, F0 to F7, from the editor
        // (MIDI_CONTROL_PORT) or the host's wire (HOST_IN). Handled now, at
        // the time the passes are at; the reply is in midi() by the time
        // this returns.
        void receive_sysex(uint8_t source, const uint8_t* data, uint32_t length);

        // The simulated time the next pass runs at.
        uint32_t now() const { return now_us; }

        PluginMidiOut& midi(){ return midi_out; }
        MixedModeMaster& master(){ return mm; }
        PatchManager& patches(){ return patch_manager; }
        const PatchStore& store() const { return patch_store; }
        const PluginLeds& leds() const { return led_driver; }
        uint32_t input_overflows() const { return queue.overflows(); }

        // State: what a host saves with its project.
        //
        // The whole of the module's memory, twice over: the EEPROM image,
        // which is slot 0 and the presets exactly as the module keeps them,
        // and the running patch encoded on its own - slot 0 is a kilobyte
        // and the running patch is allowed to be larger than that, and a
        // save must never lose the patch that is playing. Nothing is
        // migrated: a state written by another PATCH_FORMAT_VERSION is
        // refused whole, and the module comes up on its defaults with the
        // red LED lit, as a module flashed over an old EEPROM does.
        static constexpr uint32_t STATE_MAGIC = 0x4D4D5653u;   // "MMVS"
        static constexpr size_t   STATE_HEADER = 8;            // magic, image length
        // The most write_state() produces.
        static size_t state_capacity();
        // Returns the bytes written, or 0 when the running patch would not
        // encode or `capacity` is short.
        size_t write_state(uint8_t* out, size_t capacity);
        // Returns false when the bytes were not this engine's state, or the
        // patch in them did not validate; either way the module is running
        // something afterwards.
        bool read_state(const uint8_t* in, size_t length);

    private:
        void run_boot();
        void pass();
        void drive_clock();
        void enqueue(const InEvent& e);

        PluginGpio      gpio;
        PluginMidiOut   midi_out;
        PluginEeprom    eeprom;
        PluginLeds      led_driver;
        HalTap          panel;
        MixedModeMaster mm;
        StatusLeds      status;
        Monitor         monitor;
        PatchStore      patch_store;
        PatchManager    patch_manager;
        Macros          macros;
        CcMapper        cc_map;
        ControlSum      control_sum;
        ModMatrix       mod_matrix;
        NrpnDecoder     nrpn;
        SysexHandler    protocol;
        MidiInputQueue  queue;

        uint32_t now_us;
        // The interval timer: the subtick period and when the next one is
        // due, in simulated microseconds (compare emulator's driveClock).
        uint32_t subtick_interval;
        uint32_t subtick_due;
        // Where the passes are on the sample clock: the block's first sample
        // as an absolute count, and the absolute sample the next pass is at.
        double samples_per_pass;
        double block_start;
        double next_pass;
        // The beat the green LED last flashed on (main.cpp).
        uint32_t last_beat;
        bool have_beat;
        bool booted;
};

}

#endif
