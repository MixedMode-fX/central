#include "hal/teensy/teensy_includes.h"

#include "hardware.h"
#include "hal/teensy/teensy_gpio.h"
#include "hal/teensy/teensy_midi.h"
#include "hal/teensy/teensy_clock.h"
#include "hal/gpio_map.h"
#include "midi/midi_queue.h"
#include "util/random.h"
#include "master.h"
#include "version.h"

static const uint8_t GPIO_PIN_TABLE[GPIO_N] = {GPIO_PINS};
static TeensyGpio gpio(GPIO_PIN_TABLE);
static TeensyMidiOut midi_out;

// Owns the clock, the buses, the hardware port nodes and the node pool.
// Everything is allocated statically: no heap use after boot.
static MixedModeMaster master(gpio, midi_out);

// Filled by the transports, drained at the top of every pass.
static MidiInputQueue midi_in_queue;

// Until presets exist (#11): a sustain pedal on jack 8 sent everywhere, and
// the internal clock divided by four onto jack 1 so the module has a pulse
// out of the box.
static Patch default_patch(){
    Patch p = empty_patch();
    p.gate_ports[7] = GatePortConfig{GATE_PORT_IN, 0};        // jack 8 -> gate bus 0
    p.nodes[0] = node_config(ALGO_SUSTAIN);                   // gate bus 0 -> note bus 0
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 0;
    p.nodes[1] = node_config(ALGO_CLOCK_DIV);                 // master tick -> gate bus 1
    p.nodes[1].out_bus[0] = 1;
    p.nodes[1].params[1] = 4;                                 // /4: one pulse per beat
    p.n_nodes = 2;
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 1};       // gate bus 1 -> jack 1
    p.midi_out[0] = MidiOutConfig{ALL_MIDI_PORTS, 0, 0};      // note bus 0 -> every transport
    return p;
}

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

    mm_midi_setup();

    master.load(default_patch());
    master.setup();

    // Last: the timer only starts once there is a patch for it to drive.
    mm_clock_setup(master.clock());
}

void loop(){
    const uint32_t now = micros();

    // 1. transports in: parse and enqueue, nothing more.
    mm_midi_read(midi_in_queue);

    // 2. hand every queued message to the ports that want it. Realtime
    //    messages go to the clock instead of onto a bus (#4, #5).
    SourcedMidiEvent in;
    while (midi_in_queue.pop(in)) master.deliver_midi(in.source, in.event, now);

    // 3. one evaluation pass, which also collects the clock's subticks.
    master.pass(now);

    // 4. reprogram the subtick timer if the tempo or the external period moved.
    mm_clock_service();
}
