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
static Console console(console_io, patches, master, store, leds);

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
    mm_midi_read(midi_in_queue);

    // 2. hand every queued message to the ports that want it. Realtime
    //    messages go to the clock instead of onto a bus (#4, #5).
    SourcedMidiEvent in;
    while (midi_in_queue.pop(in)) master.deliver_midi(in.source, in.event, now);

    // 3. one evaluation pass, which also collects the clock's subticks.
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
    patches.service(now);
}
