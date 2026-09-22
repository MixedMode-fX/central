#ifndef MMMC_MONITOR_HAL_TAP_H
#define MMMC_MONITOR_HAL_TAP_H

#include <stdint.h>
#include "config.h"
#include "hal/igpio.h"
#include "hal/imidi_out.h"
#include "hal/midi_types.h"

// The hardware seam, with a tap on it.
//
// The module has no display, so what it is doing at its back panel - the
// level on every jack, the notes leaving on every cable - is exactly what an
// editor has to be told, and exactly what nothing in the core remembers: a
// port node reads a pin and writes a bus, and the level is gone with the
// pass. This sits between the port nodes and the real IGpio and IMidiOut,
// forwards everything untouched, and keeps what went through: the last
// level read from and written to each jack, and the note-ons and note-offs
// sent since it was last cleared. The monitor (monitor/monitor.h) reads it
// once a pass; nothing else knows it is there.
//
// It costs a store per read, a store per write and a copy per note. A jack
// nobody reads keeps whatever it last read, which is low from boot, so a
// jack no patch claims is dark.
class HalTap : public IGpio, public IMidiOut {
    public:
        struct Sent {
            uint8_t target;
            uint8_t type;
            uint8_t channel;
            uint8_t d1;
            uint8_t d2;
        };
        // Notes one pass can send before the tap stops remembering: every
        // output port emptying a full note bus. Past this a note is still
        // sent, and the monitor reports that it lost some.
        static constexpr uint16_t SENT = N_MIDI_OUT_NODES * NOTE_QUEUE_DEPTH;

        HalTap(IGpio& gpio_if, IMidiOut& midi_if) :
            gpio(gpio_if), midi(midi_if), in_bits(0), out_bits(0),
            sent(), n_sent(0), sent_lost(false) {}
        HalTap(const HalTap&) = delete;
        HalTap& operator=(const HalTap&) = delete;

        // IGpio, forwarded.
        void mode(uint8_t port, uint8_t m) override { gpio.mode(port, m); }
        void write(uint8_t port, uint8_t state) override {
            if (port < GPIO_N){
                if (state) out_bits |= (uint16_t)(1u << port); else out_bits &= (uint16_t)~(1u << port);
            }
            gpio.write(port, state);
        }
        uint8_t read(uint8_t port) override {
            const uint8_t level = gpio.read(port);
            if (port < GPIO_N){
                if (level) in_bits |= (uint16_t)(1u << port); else in_bits &= (uint16_t)~(1u << port);
            }
            return level;
        }

        // IMidiOut, forwarded. Only a note on a musical cable is kept: the
        // control cable carries the protocol, and a clock byte forty times a
        // second is not something anybody draws.
        void send(uint8_t target, uint8_t type, uint8_t d1, uint8_t d2, uint8_t channel) override {
            if ((type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) && (target & MIDI_MUSICAL_PORTS)){
                if (n_sent < SENT) sent[n_sent++] = Sent{target, type, channel, d1, d2};
                else sent_lost = true;
            }
            midi.send(target, type, d1, d2, channel);
        }
        void send_sysex(uint8_t target, const uint8_t* data, uint16_t length) override {
            midi.send_sysex(target, data, length);
        }

        // One bit per jack: what was last read at it, and last written to it.
        uint16_t jacks_in() const { return in_bits; }
        uint16_t jacks_out() const { return out_bits; }

        // The notes sent since clear_sent().
        uint16_t sent_count() const { return n_sent; }
        const Sent& sent_at(uint16_t i) const { return sent[i]; }
        bool sent_overflowed() const { return sent_lost; }
        void clear_sent(){ n_sent = 0; sent_lost = false; }

    private:
        IGpio& gpio;
        IMidiOut& midi;
        uint16_t in_bits;
        uint16_t out_bits;
        Sent sent[SENT];
        uint16_t n_sent;
        bool sent_lost;
};

static_assert(GPIO_N <= 16, "jack levels are held in a 16-bit word");

#endif
