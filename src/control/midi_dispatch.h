#ifndef MMMC_CONTROL_MIDI_DISPATCH_H
#define MMMC_CONTROL_MIDI_DISPATCH_H

#include <stdint.h>
#include "midi/midi_queue.h"
#include "master.h"
#include "protocol/sysex_handler.h"
#include "control/cc_mapper.h"
#include "control/nrpn.h"

// The path every queued MIDI message takes into the module, written once so
// the Teensy's main loop and the tests run the same code (#5, #21, #22).
//
// In order: a Program Change the module is listening for recalls a preset
// and is consumed; an NRPN controller, where NRPN is enabled for that port
// and channel, is consumed by the decoder; a mapped CC is consumed by the
// binding table (unless flagged pass-through); everything left is offered
// to the MidiInPorts, and system realtime goes to the clock.
//
// **The drain stops in front of a message the graph has no room for.** A
// note bus holds NOTE_QUEUE_DEPTH events per pass, and the transports can
// deliver more than that in one loop - a sustain pedal releasing twenty
// notes arrives as twenty note-offs in one USB packet. Delivering past the
// depth would count the tail as overflow and hang those notes downstream,
// so instead the message stays queued and the next pass takes it: one pass
// of latency for the tail of a burst, and nothing lost. The input queue is
// MIDI_INPUT_QUEUE_DEPTH deep, which is the real ceiling.
//
// Returns how many messages left the queue.
uint16_t dispatch_midi(MidiInputQueue& queue, SysexHandler& protocol, NrpnDecoder& nrpn,
                       CcMapper& cc, MixedModeMaster& master, uint32_t now_us);

#endif
