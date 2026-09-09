#ifndef MMMC_MIDI_MIDI_QUEUE_H
#define MMMC_MIDI_MIDI_QUEUE_H

#include <stdint.h>
#include "config.h"
#include "bus/domain.h"

// One incoming message, carrying the transport it arrived on (#5).
// A node never learns which wire a message came from - only MidiInPort does,
// and only to decide whether the message is its.
struct SourcedMidiEvent {
    uint8_t source;      // one MidiPort bit
    MidiEvent event;
};

// The transport-to-pass buffer.
//
// The transport side enqueues and does nothing else: parsing a note-on must
// not run an algorithm, both because a callback may be an interrupt and
// because the pass is where evaluation order is defined. The main loop drains
// the queue at the top of each pass, in step 1 of the evaluation order.
//
// A full queue drops the newest message and counts it. Dropping the newest
// keeps what is already queued in order, and the counter is what turns "MIDI
// went strange under load" into a number somebody can read (#7's console).
class MidiInputQueue {
    public:
        MidiInputQueue() : items(), head(0), tail(0), dropped(0) {}

        bool push(uint8_t source, const MidiEvent& event){
            const uint8_t next = (uint8_t)((head + 1u) % MIDI_INPUT_QUEUE_DEPTH);
            if (next == tail){
                dropped++;
                return false;
            }
            items[head].source = source;
            items[head].event = event;
            head = next;
            return true;
        }

        bool pop(SourcedMidiEvent& out){
            if (tail == head) return false;
            out = items[tail];
            tail = (uint8_t)((tail + 1u) % MIDI_INPUT_QUEUE_DEPTH);
            return true;
        }
        // The oldest message without taking it, so the drain can stop in
        // front of a message the graph has no room for and leave it queued
        // for the next pass (control/midi_dispatch.h).
        bool peek(SourcedMidiEvent& out) const {
            if (tail == head) return false;
            out = items[tail];
            return true;
        }

        bool empty() const { return head == tail; }
        uint8_t count() const {
            return (uint8_t)((head + MIDI_INPUT_QUEUE_DEPTH - tail) % MIDI_INPUT_QUEUE_DEPTH);
        }
        uint32_t overflows() const { return dropped; }
        void clear(){ head = tail = 0; }

    private:
        SourcedMidiEvent items[MIDI_INPUT_QUEUE_DEPTH];
        volatile uint8_t head;   // written by the transport side only
        volatile uint8_t tail;   // written by the main loop only
        volatile uint32_t dropped;
};

static_assert(MIDI_INPUT_QUEUE_DEPTH <= 255, "queue indices are 8-bit");

#endif
