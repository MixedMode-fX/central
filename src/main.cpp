#include "hal/teensy/teensy_includes.h"

#include "hardware.h"
#include "hal/teensy/teensy_gpio.h"
#include "hal/teensy/teensy_midi.h"
#include "version.h"

#include "algorithm/switch/sustain.h"
#include "algorithm/logic/gates.h"

static const uint8_t GPIO_PIN_TABLE[GPIO_N] = {GPIO_PINS};
static TeensyGpio gpio(GPIO_PIN_TABLE);
static TeensyMidiOut midi_out;

// Algorithms are allocated statically: no heap use after boot, and their
// destructors (which return ports to a safe state) can actually run.
static Sustain sustain(gpio, midi_out, 0b10000000, ALL_MIDI_PORTS);

void setup(){
    Serial.begin(115200);
    Serial.println("MMMC " MMMC_BUILD);

    // Every port starts as an input; an algorithm claims an output in setup().
    gpio_map_mode(gpio, ALL_GPIO_MAP, GPIO_MODE_INPUT_PULLUP);

    mm_midi_setup();

    sustain.setup();
}

void loop(){
    mm_midi_read();
    sustain.update(micros());
}
