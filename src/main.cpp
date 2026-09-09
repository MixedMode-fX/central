#include "hal/teensy/teensy_includes.h"

#include "hardware.h"
#include "hal/teensy/teensy_gpio.h"
#include "hal/teensy/teensy_midi.h"
#include "hal/teensy/teensy_clock.h"
#include "hal/teensy/teensy_eeprom.h"
#include "hal/teensy/teensy_leds.h"
#include "hal/teensy/teensy_console_io.h"
#include "hal/gpio_map.h"
#include "midi/midi_queue.h"
#include "util/random.h"
#include "master.h"
#include "led/status_leds.h"
#include "patch/patch_store.h"
#include "patch/patch_manager.h"
#include "console/console.h"
#include "protocol/sysex_handler.h"
#include "control/cc_mapper.h"
#include "control/nrpn.h"
#include "hal/midi_types.h"
#include "version.h"

static const uint8_t GPIO_PIN_TABLE[GPIO_N] = {GPIO_PINS};
static TeensyGpio gpio(GPIO_PIN_TABLE);
static TeensyMidiOut midi_out;
static TeensyEeprom eeprom;
static TeensyLeds led_driver;
static TeensyConsoleIo console_io;

// Owns the clock, the buses, the hardware port nodes and the node pool.
// Everything is allocated statically: no heap use after boot.
static MixedModeMaster master(gpio, midi_out);

// The module's entire feedback surface (#7): two LEDs and a text console.
static StatusLeds leds(led_driver);
static PatchStore store(eeprom);
static PatchManager patches(master, store, leds);

// Controller bindings (#21). Not a node: a parameter is not a bus signal, so
// mapping applies at the MIDI input layer and writes through the same
// validated entry point the protocol and the console use.
static CcMapper cc_map(patches, master);
static NrpnDecoder nrpn(patches, cc_map);
static Console console(console_io, patches, master, store, leds, cc_map);

// The patch protocol (#11). Not a node, not reachable from a bus: with no
// button to hold at power-on, a patch that could take this down would leave
// reflashing over USB as the only way to recover.
static SysexHandler protocol(patches, master, store, leds, midi_out, cc_map);

// Filled by the transports, drained at the top of every pass.
static MidiInputQueue midi_in_queue;

// The last beat the green LED flashed on, so a flash happens once per beat
// rather than once per subtick.
static uint32_t last_beat = 0;
static bool have_beat = false;

void setup(){
    Serial.begin(115200);
    Serial.println("MMMC " MMMC_BUILD);

    // Stir the entropy pool before any node is constructed: a node that draws
    // a seed at construction (RandomSequencer, Probability) must not play the
    // same thing on every power cycle. The cycle counter and a floating ADC
    // input are both weak on their own and differ between boots.
    entropy::stir(ARM_DWT_CYCCNT);
    entropy::stir(micros());
    for (uint8_t i = 0; i < 8; i++) entropy::stir((uint32_t)analogRead(CV_ADC) << i);

    // Every port starts as an input; a port node claims an output in setup().
    gpio_map_mode(gpio, ALL_GPIO_MAP, GPIO_MODE_INPUT_PULLUP);

    led_driver.begin();
    console_io.begin();
    mm_midi_setup();

    // Slot 0 if it checks out, the flash default if it does not - and the red
    // LED says which (#7). A freshly flashed module with an empty EEPROM
    // still comes up doing something observable.
    patches.boot(micros());
    console.greet();

    // Last: the timer only starts once there is a patch for it to drive.
    mm_clock_setup(master.clock());
}

void loop(){
    const uint32_t now = micros();

    // 1. transports in: parse and enqueue, nothing more.
    mm_midi_read(midi_in_queue, protocol);

    // 2. hand every queued message to the ports that want it. Realtime
    //    messages go to the clock instead of onto a bus (#4, #5).
    SourcedMidiEvent in;
    while (midi_in_queue.pop(in)){
        // A Program Change the module is listening for recalls a preset and
        // is consumed; every other one carries on to the graph, so a Program
        // Change meant for a downstream synth is not silently swallowed.
        if (in.event.type == MIDI_PROGRAM_CHANGE &&
            protocol.program_change(in.source, in.event.channel, in.event.data1, now)) continue;
        // A mapped CC is a control-plane write, not a bus event. It is
        // remembered here and applied once at the pass boundary, so a knob
        // sweep costs one parameter write per mapping however fast it is
        // sent. A mapping flagged pass-through also reaches the graph.
        // NRPN first, and only where it is enabled: 99/98/6/38 look like
        // ordinary CCs, so a module that always consumed them would silently
        // eat a stream meant for a downstream synth (#22).
        if (in.event.type == MIDI_CONTROL_CHANGE &&
            nrpn.observe(in.source, in.event.channel, in.event.data1, in.event.data2, now)) continue;
        if (in.event.type == MIDI_CONTROL_CHANGE &&
            cc_map.observe(in.source, in.event.channel, in.event.data1, in.event.data2, now)) continue;
        master.deliver_midi(in.source, in.event, now);
    }

    // 3. apply whatever the controllers moved, then one evaluation pass.
    //    Parameter writes happen here, before process(), and never in an
    //    interrupt: the transport enqueues, the pass applies.
    cc_map.apply(now);
    master.pass(now);

    // 4. reprogram the subtick timer if the tempo or the external period moved.
    mm_clock_service();

    // 5. the feedback surface. All of this is after the signal path and none
    //    of it blocks: the LEDs are two writes, the console reads whatever
    //    bytes are waiting, and the store writes at most once every couple of
    //    seconds and only when something changed.
    const uint32_t beat = master.clock().count() / CLOCK_SUBTICKS_PER_QUARTER;
    if (!have_beat || beat != last_beat){
        if (have_beat) leds.beat(now);
        last_beat = beat;
        have_beat = true;
    }
    leds.set_clock_running(master.clock().running());
    leds.service(now);
    console.service(now);
    protocol.service(now);
    nrpn.service(now);
    patches.service(now);
}
