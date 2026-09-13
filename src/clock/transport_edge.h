#ifndef MMMC_CLOCK_TRANSPORT_EDGE_H
#define MMMC_CLOCK_TRANSPORT_EDGE_H

#include <stdint.h>

// The three things that happen to the transport, as a mask.
//
// A start, a stop and a continue are *events*, not a state: "running" is
// MasterClock::running(), and the count going back to zero is what a start
// leaves behind, but neither of those tells a node that the transport just
// moved, and nothing tells it apart from a continue at all. This is the
// vocabulary the clock latches them in and the master hands them to the pool
// in (node/node.h), which is why it is a header of its own rather than a
// member of either: MasterClock is below Node and Node is above it.
//
// A mask rather than a queue. Two transport messages inside one pass - a DAW
// sending stop and start back to back - are a millisecond apart, and what a
// patch does with them is fire both edges; keeping their order would cost a
// queue in every node for a distinction nothing downstream can hear.
enum TransportEdge : uint8_t {
    TRANSPORT_START    = 1u << 0,   // MIDI Start: run from the top, count at zero
    TRANSPORT_STOP     = 1u << 1,   // MIDI Stop
    TRANSPORT_CONTINUE = 1u << 2,   // MIDI Continue: run again where it stopped
};

#endif
