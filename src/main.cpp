#include "hal/teensy/teensy_includes.h"

#include "hardware.h"
#include "hal/teensy/teensy_gpio.h"
#include "hal/teensy/teensy_midi.h"
#include "hal/gpio_map.h"
#include "master.h"
#include "version.h"

static const uint8_t GPIO_PIN_TABLE[GPIO_N] = {GPIO_PINS};
static TeensyGpio gpio(GPIO_PIN_TABLE);
static TeensyMidiOut midi_out;

// Owns the buses, the hardware port nodes and the node pool. Everything is
// allocated statically: no heap use after boot.
static MixedModeMaster master(gpio, midi_out);

// Until presets exist (#11): a sustain pedal on jack 8, sent everywhere.
static Patch default_patch(){
    Patch p = empty_patch();
    p.gate_ports[7] = GatePortConfig{GATE_PORT_IN, 0};        // jack 8 -> gate bus 0
    p.nodes[0] = node_config(ALGO_SUSTAIN);                   // gate bus 0 -> note bus 0
    p.nodes[0].in_bus[0] = 0;
    p.nodes[0].out_bus[0] = 0;
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{ALL_MIDI_PORTS, 0, 0};      // note bus 0 -> every transport
    return p;
}

void setup(){
    Serial.begin(115200);
    Serial.println("MMMC " MMMC_BUILD);

    // Every port starts as an input; a port node claims an output in setup().
    gpio_map_mode(gpio, ALL_GPIO_MAP, GPIO_MODE_INPUT_PULLUP);

    mm_midi_setup();

    master.load(default_patch());
    master.setup();
}

void loop(){
    mm_midi_read();
    master.pass(micros());
}
