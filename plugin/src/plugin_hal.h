#ifndef MMMC_PLUGIN_PLUGIN_HAL_H
#define MMMC_PLUGIN_PLUGIN_HAL_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"
#include "hal/ieeprom.h"
#include "hal/ileds.h"
#include "hal/midi_types.h"
#include "patch/patch_store.h"

// The plugin's implementations of the hardware seams: the third set, beside
// src/hal/teensy/ and emulator/src/web_hal.h. A DAW is the hardware here,
// and it has exactly one thing a module's back panel has: MIDI.

// **There are no jacks.** The plugin's outputs are MIDI and nothing else,
// so a gate port in the patch is a port on no wire: an output writes into
// the void, an input reads a level nobody drives. The patch format keeps
// the jacks - it is the module's format, not the plugin's - and a patch
// that uses one still validates, plays, and does nothing at that jack.
class PluginGpio : public IGpio {
    public:
        void mode(uint8_t, uint8_t) override {}
        void write(uint8_t, uint8_t) override {}
        uint8_t read(uint8_t) override { return GPIO_LOW; }
};

// One message on its way out of the plugin, stamped with the sample of the
// block it belongs at. `length` is 1 for realtime, 2 for Program Change and
// channel aftertouch, 3 for everything else, so a host wrapper builds the
// right message without a table of its own.
struct PluginMidiEvent {
    uint32_t sample;
    uint8_t  status;
    uint8_t  d1;
    uint8_t  d2;
    uint8_t  length;
};

// IMidiOut with **every cable on one wire.** The firmware routes by an
// eight-bit target mask - four USB cables, DIN ports, a host port - and a
// plugin has one MIDI output, so any musical bit at all reaches the host and
// the mask is otherwise ignored. That is the whole of "output routing is just
// MIDI": a MidiOutPort's target is which *note buses* it sends, and where
// they go is the DAW's business.
//
// The control cable is the one exception, in one direction. A SysEx reply
// addressed to MIDI_CONTROL_PORT is the protocol answering the editor, which
// is in the plugin's own window rather than on the host's wire, so it is
// buffered here for the editor to read - the way emulator/src/web_hal.h holds
// replies for the page. A reply addressed to a musical cable came from a
// request on the host's MIDI input, and goes back out the same way.
class PluginMidiOut : public IMidiOut {
    public:
        // Messages one block can carry. A pass may emit up to
        // N_MIDI_OUT_NODES * NOTE_QUEUE_DEPTH events plus clock, and a block
        // is a dozen passes at most; past this the newest is dropped and
        // counted rather than the block growing.
        static constexpr uint32_t EVENTS = 1024;
        // SysEx to the host, per block. A dump is a few chunks of under
        // SYSEX_TX_MAX bytes; the algorithm list is the largest burst.
        static constexpr uint32_t HOST_SYSEX = 16384;
        // SysEx to the editor. Sized as the emulator sizes its buffer, and
        // for the same burst.
        static constexpr uint32_t EDITOR_SYSEX = 16384;

        PluginMidiOut() :
            sample(0), events(), n_events(0), dropped(0),
            host_sysex(), host_sysex_used(0), host_sysex_dropped(0),
            editor_sysex(), editor_sysex_used(0), editor_sysex_dropped(0) {}

        // The sample every message sent from now on is stamped with: the
        // engine sets it before each pass.
        void at(uint32_t block_sample){ sample = block_sample; }

        void send(uint8_t target, uint8_t type, uint8_t d1, uint8_t d2, uint8_t channel) override {
            if ((target & MIDI_MUSICAL_PORTS) == 0) return;
            if (n_events >= EVENTS){ dropped++; return; }
            PluginMidiEvent& e = events[n_events++];
            e.sample = sample;
            if (type >= 0xF0){
                e.status = type; e.d1 = 0; e.d2 = 0; e.length = 1;
            } else {
                e.status = (uint8_t)((type & 0xF0) | ((channel - 1u) & 0x0Fu));
                e.d1 = (uint8_t)(d1 & 0x7F);
                e.d2 = (uint8_t)(d2 & 0x7F);
                const uint8_t kind = (uint8_t)(type & 0xF0);
                e.length = (kind == MIDI_PROGRAM_CHANGE || kind == MIDI_AFTERTOUCH_CHANNEL) ? 2 : 3;
            }
        }

        void send_sysex(uint8_t target, const uint8_t* data, uint16_t length) override {
            if (target & MIDI_CONTROL_PORT){
                append(editor_sysex, EDITOR_SYSEX, editor_sysex_used, editor_sysex_dropped, data, length);
            }
            if (target & MIDI_MUSICAL_PORTS){
                append(host_sysex, HOST_SYSEX, host_sysex_used, host_sysex_dropped, data, length);
            }
        }

        // The host wrapper takes a block's messages, then clears for the next.
        void clear_events(){ n_events = 0; }
        void clear_host_sysex(){ host_sysex_used = 0; }
        void clear_editor_sysex(){ editor_sysex_used = 0; }

        uint32_t sample;
        PluginMidiEvent events[EVENTS];
        uint32_t n_events;
        uint32_t dropped;
        // SysEx replies, each F0..F7 inclusive, concatenated in send order.
        uint8_t  host_sysex[HOST_SYSEX];
        uint32_t host_sysex_used;
        uint32_t host_sysex_dropped;
        uint8_t  editor_sysex[EDITOR_SYSEX];
        uint32_t editor_sysex_used;
        uint32_t editor_sysex_dropped;

    private:
        static void append(uint8_t* buffer, uint32_t capacity, uint32_t& used, uint32_t& lost,
                           const uint8_t* data, uint16_t length){
            if (used + length > capacity){ lost++; return; }
            for (uint16_t i = 0; i < length; i++) buffer[used++] = data[i];
        }
};

// IEeprom in RAM, the size of the Teensy's, so "does this patch fit a
// slot?" has the module's answer. It does not survive on its own: the host
// keeps it, as part of the plugin's state, and hands it back with the
// project (engine.h).
class PluginEeprom : public IEeprom {
    public:
        PluginEeprom() : cells() {
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

// ILeds over two bytes. Nothing draws them yet; they are kept so the
// module's feedback vocabulary is there for a window that wants it.
class PluginLeds : public ILeds {
    public:
        PluginLeds() : levels() {}
        void set(uint8_t led, uint8_t brightness) override {
            if (led < LED_COUNT) levels[led] = brightness;
        }
        uint8_t levels[LED_COUNT];
};

#endif
