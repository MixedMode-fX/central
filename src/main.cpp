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

void setup(){
    Serial.begin(115200);
    Serial.println("MMMC " MMMC_BUILD);

    // GPIO Initialisation
    gpio_map_mode(gpio, ALL_GPIO_MAP, GPIO_MODE_OUTPUT);
    gpio_map_write(gpio, ALL_GPIO_MAP, GPIO_LOW);

    // MIDI Initialisation
    mm_midi_setup();
}

void loop(){
    Algorithm* sustain = new Sustain(gpio, midi_out, 0, ALL_MIDI_PORTS, 0b10000000, 0);
    while(true){
        mm_midi_read();
        sustain->update();
    }
}
