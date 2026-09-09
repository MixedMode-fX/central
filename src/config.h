#ifndef MMMC_CONFIG_H
#define MMMC_CONFIG_H

// Framework-free sizing constants shared by the firmware and the native tests.
// Anything that names a physical pin or a transport lives in hardware.h, which
// is only ever included by the Teensy hardware layer (src/hal/teensy/).
//
// See README "Signal bus model" for the reasoning behind these numbers.

#include <stdint.h>

// Number of front-panel I/O ports (jacks).
#define GPIO_N 8
// Bitmask selecting every port.
#define ALL_GPIO_MAP ((uint16_t)((1u << GPIO_N) - 1u))

// Internal buses per domain.
#define N_GATE_BUS 16
#define N_NOTE_BUS 8
#define N_CV_BUS 8
// Events a note bus can carry per pass. Beyond this, writes are dropped and
// counted (BusManager::note_overflows()).
#define NOTE_QUEUE_DEPTH 16

// Node pool: uniform slots, each large enough for any algorithm's state.
#define N_NODE 32
#define NODE_SLOT_SIZE 320

// Per-node connection limits (NodeConfig is also the preset format).
#define MAX_IN 4
#define MAX_OUT 8
#define N_PARAM 8

// Reserved hardware nodes outside the pool.
#define N_MIDI_IN_NODES 4
#define N_MIDI_OUT_NODES 4

#endif
