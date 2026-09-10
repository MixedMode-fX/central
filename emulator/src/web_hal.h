#ifndef MMMC_EMULATOR_WEB_HAL_H
#define MMMC_EMULATOR_WEB_HAL_H

#include <stdint.h>
#include "config.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"
#include "hal/ieeprom.h"
#include "hal/ileds.h"
#include "patch/patch_store.h"

// The browser's implementations of the two hardware seams. They are the
// emulator's equivalent of src/hal/teensy/: the jacks are bytes the page
// can read and write, and MIDI out is a call into JavaScript.

// IGpio over eight bytes. The page sets input levels; the firmware's port
// nodes set modes and output levels; the page reads them back.
class WebGpio : public IGpio {
    public:
        WebGpio() : modes(), inputs(), outputs() {}

        void mode(uint8_t port, uint8_t m) override { if (port < GPIO_N) modes[port] = m; }
        void write(uint8_t port, uint8_t state) override { if (port < GPIO_N) outputs[port] = state; }
        // Levels are already normalised: the page supplies "gate present"
        // directly, the way TeensyGpio::read() returns it after polarity.
        uint8_t read(uint8_t port) override { return port < GPIO_N ? inputs[port] : GPIO_LOW; }

        uint8_t modes[GPIO_N];
        uint8_t inputs[GPIO_N];
        uint8_t outputs[GPIO_N];
};

// Supplied by the page (see index.html: the "env" import object).
extern "C" __attribute__((import_module("env"), import_name("mmmc_midi_send")))
void mmmc_midi_send(uint32_t target, uint32_t type, uint32_t d1, uint32_t d2, uint32_t channel);

// IMidiOut that hands every message to JavaScript with its target mask.
//
// SysEx replies are buffered rather than handed over one call at a time: the
// page reads them out through emu_api, the same way it reads jack levels, so
// the emulator needs no second import and a patch editor running against it
// sees exactly the bytes the firmware would put on the wire.
class WebMidiOut : public IMidiOut {
    public:
        // Sized by the largest burst the firmware sends in one go, which is
        // the algorithm list: one reply per algorithm, each carrying its port
        // names and its summary, at up to SYSEX_TX_MAX bytes. Four kilobytes
        // held thirty-one of them and silently dropped the rest, which read
        // in the app as "the answer never completed" rather than as an
        // overflow. Sixteen covers a hundred and twenty algorithms, and this
        // is a buffer in a browser rather than anything the module has to
        // find room for - the hardware streams to a MIDI port.
        static constexpr uint32_t SYSEX_BUFFER = 16384;

        WebMidiOut() : sysex_bytes(), sysex_used(0), sysex_dropped(0) {}

        void send(uint8_t target, uint8_t type, uint8_t d1, uint8_t d2, uint8_t channel) override {
            mmmc_midi_send(target, type, d1, d2, channel);
        }

        void send_sysex(uint8_t, const uint8_t* data, uint16_t length) override {
            if (sysex_used + length > SYSEX_BUFFER){ sysex_dropped++; return; }
            for (uint16_t i = 0; i < length; i++) sysex_bytes[sysex_used++] = data[i];
        }

        void drain_sysex(){ sysex_used = 0; }

        uint8_t sysex_bytes[SYSEX_BUFFER];
        uint32_t sysex_used;
        uint32_t sysex_dropped;
};

// IEeprom in RAM. The browser has no flash to emulate, and a page that
// reloaded would lose its presets anyway, so this is a plain byte array the
// same size as the Teensy's - which is what makes "does this patch fit a
// slot?" answerable in the emulator (#7).
class WebEeprom : public IEeprom {
    public:
        WebEeprom() : cells() {
            for (size_t i = 0; i < sizeof cells; i++) cells[i] = 0xFF;
        }
        size_t size() const override { return sizeof cells; }
        uint8_t read(size_t address) const override {
            return address < sizeof cells ? cells[address] : 0xFF;
        }
        void write(size_t address, uint8_t value) override {
            if (address < sizeof cells) cells[address] = value;
        }
        uint8_t cells[EEPROM_BYTES];
    };

// ILeds over two bytes the page reads back, so the emulator shows the same
// vocabulary the module's two LEDs do (#7).
class WebLeds : public ILeds {
    public:
        WebLeds() : levels() {}
        void set(uint8_t led, uint8_t brightness) override {
            if (led < LED_COUNT) levels[led] = brightness;
        }
        uint8_t levels[LED_COUNT];
    };

#endif
