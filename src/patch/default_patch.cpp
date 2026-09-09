#include "patch/default_patch.h"
#include "node/registry.h"
#include "hal/midi_types.h"

// One pulse per quarter note from the tick source: ClockDiv's amount is in
// PPQN ticks and the master runs at MASTER_PPQN of them per quarter, so
// dividing by MASTER_PPQN is exactly the beat.
static_assert(MASTER_PPQN <= 255, "the metronome divisor has to fit a parameter byte");

Patch default_patch(){
    Patch p = empty_patch();

    // MIDI thru: every transport in, omni, onto note bus 0; note bus 0 back
    // out to every musical transport. The control cable is not in the mask -
    // a patch cannot reroute the protocol's own port (#11).
    p.midi_in[0]  = MidiInConfig{MIDI_MUSICAL_PORTS, 0, 0};
    p.midi_out[0] = MidiOutConfig{MIDI_MUSICAL_PORTS, 0, 0};

    // A sustain pedal on jack 8, sent to the same bus, so a pedal plugged in
    // works with no configuration at all. An unpatched input reads as no
    // gate, so an empty jack sends nothing.
    p.gate_ports[7] = GatePortConfig{GATE_PORT_IN, 0};        // jack 8 -> gate bus 0
    p.nodes[0] = node_config(ALGO_SUSTAIN);
    p.nodes[0].in_bus[0] = 0;                                 // gate bus 0
    p.nodes[0].out_bus[0] = 0;                                // note bus 0

    // A metronome on jack 1: the internal clock, one trigger per beat, so a
    // module with nothing patched into it still shows a pulse.
    p.nodes[1] = node_config(ALGO_CLOCK_DIV);
    p.nodes[1].out_bus[0] = 1;                                // gate bus 1
    p.nodes[1].params[1] = MASTER_PPQN;                       // one pulse per quarter
    p.gate_ports[0] = GatePortConfig{GATE_PORT_OUT, 1};       // gate bus 1 -> jack 1

    p.n_nodes = 2;
    return p;
}

GlobalSettings default_globals_for_patch(){
    GlobalSettings g = default_globals();
    g.clock_source = 0;                   // MasterClock::CLOCK_INTERNAL
    g.bpm = CLOCK_DEFAULT_BPM;
    // Program Change recall stays off until a user asks for it: a Program
    // Change meant for a downstream synth must not silently switch the patch
    // of a module nobody has configured (#11).
    g.pc_enabled = 0;
    return g;
}
