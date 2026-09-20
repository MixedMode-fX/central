#include "clock/clock_out.h"
#include "clock/transport_edge.h"
#include "hal/midi_types.h"

MidiClockOut::MidiClockOut() : out(nullptr), target(0), due(0), phased(false) {}

void MidiClockOut::attach(IMidiOut& transport){
    out = &transport;
}

void MidiClockOut::set_target(uint8_t target_mask){
    const uint8_t wanted = (uint8_t)(target_mask & MIDI_MUSICAL_PORTS);
    // Taking a cable into use is where the phase is unknown; widening or
    // narrowing a mask that was already sending keeps it, so adding a second
    // synth to a running chain does not re-phase the one already in time.
    if (target == 0 && wanted != 0) phased = false;
    target = wanted;
}

uint32_t MidiClockOut::next_boundary(uint32_t count){
    const uint32_t over = count % (uint32_t)CLOCK_SUBTICK;
    return over ? count + ((uint32_t)CLOCK_SUBTICK - over) : count;
}

// Realtime carries no data and no channel, and IMidiOut takes both: a
// transport writes the status byte alone (runtime/midiout.js and
// hal/teensy/teensy_midi.cpp agree on that), so what is passed for them is
// never read.
void MidiClockOut::send(uint8_t type){
    out->send(target, type, 0, 0, 1);
}

void MidiClockOut::pass(uint32_t count, bool running, uint8_t edges){
    if (out == nullptr || target == 0) return;
    if (!phased){ due = next_boundary(count); phased = true; }

    // A start puts the counter back on subtick zero, so the next clock byte
    // is owed *there* - and the loop below sends it in this same pass, which
    // is what makes the first F8 the downbeat the start announced. A continue
    // leaves the count where the stop left it, so the next byte falls on the
    // next boundary above it rather than immediately.
    if (edges & TRANSPORT_START){ send(MIDI_START); due = 0; }
    if (edges & TRANSPORT_CONTINUE){ send(MIDI_CONTINUE); due = next_boundary(count); }
    if (edges & TRANSPORT_STOP) send(MIDI_STOP);

    // Nothing is sent while stopped. The counter does not move then either
    // (MasterClock::advance), so this only matters for the pass a stop
    // arrives on: the bytes for the subticks before it have already gone.
    if (!running) return;

    uint8_t sent = 0;
    while (count >= due && sent < MAX_BURST){
        send(MIDI_CLOCK);
        due += (uint32_t)CLOCK_SUBTICK;
        sent++;
    }
    // Still owing after the cap: the gap was too long to play back, so the
    // phase is taken from where the clock actually is and the arrears are
    // dropped. The alternative is a burst that arrives as a tempo spike on
    // everything downstream.
    if (count >= due) due = next_boundary(count + 1);
}
