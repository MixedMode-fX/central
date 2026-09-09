// The emulator's C ABI: what the page calls into and nothing else.
//
// This file owns one MixedModeMaster over the web HAL and one Patch under
// construction. The page never sees a struct: it builds the patch through
// the emu_patch_* setters, reads the registry through emu_algo_*, and reads
// firmware constants through emu_const_*, so a change to config.h or to the
// preset format is picked up by rebuilding, never by editing JavaScript.
//
// Everything here is a thin call into the firmware's own code. The
// validator that accepts or rejects a patch is MixedModeMaster::load(); the
// algorithms are the ones in src/algorithm/; the buses are BusManager.

#include <stdint.h>
#include "config.h"
#include "master.h"
#include "version.h"
#include "util/random.h"
#include "web_hal.h"

#define EMU_EXPORT extern "C" __attribute__((visibility("default")))

static WebGpio gpio;
static WebMidiOut midi;
static MixedModeMaster master(gpio, midi);
static Patch patch = empty_patch();

// Build identity ----------------------------------------------------------

EMU_EXPORT const char* emu_version(){ return MMMC_BUILD; }

// Firmware constants ------------------------------------------------------

EMU_EXPORT uint32_t emu_const_gpio_n(){ return GPIO_N; }
EMU_EXPORT uint32_t emu_const_n_gate_bus(){ return N_GATE_BUS; }
EMU_EXPORT uint32_t emu_const_n_note_bus(){ return N_NOTE_BUS; }
EMU_EXPORT uint32_t emu_const_n_cv_bus(){ return N_CV_BUS; }
EMU_EXPORT uint32_t emu_const_n_node(){ return N_NODE; }
EMU_EXPORT uint32_t emu_const_max_in(){ return MAX_IN; }
EMU_EXPORT uint32_t emu_const_max_out(){ return MAX_OUT; }
EMU_EXPORT uint32_t emu_const_n_param(){ return N_PARAM; }
EMU_EXPORT uint32_t emu_const_n_midi_in(){ return N_MIDI_IN_NODES; }
EMU_EXPORT uint32_t emu_const_n_midi_out(){ return N_MIDI_OUT_NODES; }
EMU_EXPORT uint32_t emu_const_no_bus(){ return NO_BUS; }
EMU_EXPORT uint32_t emu_const_master_ppqn(){ return MASTER_PPQN; }
EMU_EXPORT uint32_t emu_const_clock_subtick(){ return CLOCK_SUBTICK; }
EMU_EXPORT uint32_t emu_const_min_bpm(){ return CLOCK_MIN_BPM; }
EMU_EXPORT uint32_t emu_const_max_bpm(){ return CLOCK_MAX_BPM; }
EMU_EXPORT uint32_t emu_const_max_sequence_len(){ return MAX_SEQUENCE_LEN; }

// Registry ----------------------------------------------------------------

EMU_EXPORT uint32_t emu_algo_count(){ return registry::count(); }
EMU_EXPORT uint32_t emu_algo_id(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->id : 0; }
EMU_EXPORT const char* emu_algo_name(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->name : ""; }
EMU_EXPORT uint32_t emu_algo_n_in(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->n_in : 0; }
EMU_EXPORT uint32_t emu_algo_min_in(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->min_in : 0; }
EMU_EXPORT uint32_t emu_algo_n_out(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->n_out : 0; }
EMU_EXPORT uint32_t emu_algo_n_params(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->n_params : 0; }
EMU_EXPORT uint32_t emu_algo_wants_tick(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? (d->wants_tick ? 1 : 0) : 0; }
EMU_EXPORT uint32_t emu_algo_state_size(uint32_t i){ const AlgorithmDescriptor* d = registry::at((uint8_t)i); return d ? d->state_size : 0; }
// Domain as its enum value: 0 Gate, 1 Note, 2 CV.
EMU_EXPORT uint32_t emu_algo_in_domain(uint32_t i, uint32_t k){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    return (d && k < d->n_in) ? (uint32_t)d->in_domain[k] : 0;
}
EMU_EXPORT uint32_t emu_algo_out_domain(uint32_t i, uint32_t k){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    return (d && k < d->n_out) ? (uint32_t)d->out_domain[k] : 0;
}

// Patch under construction ------------------------------------------------

EMU_EXPORT void emu_patch_reset(){ patch = empty_patch(); }
EMU_EXPORT void emu_patch_gate_port(uint32_t port, uint32_t direction, uint32_t bus){
    if (port < GPIO_N) patch.gate_ports[port] = GatePortConfig{(uint8_t)direction, (uint8_t)bus};
}
EMU_EXPORT void emu_patch_midi_in(uint32_t i, uint32_t source_mask, uint32_t channel, uint32_t bus){
    if (i < N_MIDI_IN_NODES) patch.midi_in[i] = MidiInConfig{(uint8_t)source_mask, (uint8_t)channel, (uint8_t)bus};
}
EMU_EXPORT void emu_patch_midi_out(uint32_t i, uint32_t target_mask, uint32_t channel, uint32_t bus){
    if (i < N_MIDI_OUT_NODES) patch.midi_out[i] = MidiOutConfig{(uint8_t)target_mask, (uint8_t)channel, (uint8_t)bus};
}
// Resets node i to node_config(algorithm_id); n_nodes grows to include it.
EMU_EXPORT void emu_patch_node(uint32_t i, uint32_t algorithm_id){
    if (i >= N_NODE) return;
    patch.nodes[i] = node_config((uint8_t)algorithm_id);
    if (patch.n_nodes <= i) patch.n_nodes = (uint8_t)(i + 1);
}
EMU_EXPORT void emu_patch_node_in(uint32_t i, uint32_t k, uint32_t bus){
    if (i < N_NODE && k < MAX_IN) patch.nodes[i].in_bus[k] = (uint8_t)bus;
}
EMU_EXPORT void emu_patch_node_out(uint32_t i, uint32_t k, uint32_t bus){
    if (i < N_NODE && k < MAX_OUT) patch.nodes[i].out_bus[k] = (uint8_t)bus;
}
EMU_EXPORT void emu_patch_node_param(uint32_t i, uint32_t k, uint32_t value){
    if (i < N_NODE && k < N_PARAM) patch.nodes[i].params[k] = (uint8_t)value;
}
EMU_EXPORT void emu_patch_n_nodes(uint32_t n){ patch.n_nodes = (uint8_t)(n > N_NODE ? N_NODE : n); }

// Loads the patch under construction. Returns LoadError; on success the
// hardware nodes have run setup() and the patch is live.
EMU_EXPORT uint32_t emu_load(){
    const LoadError e = master.load(patch);
    if (e == LOAD_OK) master.setup();
    return e;
}
EMU_EXPORT void emu_unload(){ master.unload(); }
EMU_EXPORT uint32_t emu_last_error(){ return master.last_error(); }
EMU_EXPORT uint32_t emu_last_node_error(){ return master.last_node_error(); }
EMU_EXPORT uint32_t emu_last_node_index(){ return master.last_node_index(); }
EMU_EXPORT uint32_t emu_node_count(){ return master.node_count(); }

// Running -----------------------------------------------------------------

EMU_EXPORT void emu_pass(uint32_t now_us){ master.pass(now_us); }
// Offers a message to every MidiInPort, as main.cpp does when it drains the
// transport queue. System realtime messages go to the clock instead.
EMU_EXPORT uint32_t emu_deliver_midi(uint32_t source, uint32_t type, uint32_t channel, uint32_t d1, uint32_t d2, uint32_t now_us){
    const MidiEvent e = {(uint8_t)type, (uint8_t)channel, (uint8_t)d1, (uint8_t)d2};
    return master.deliver_midi((uint8_t)source, e, now_us);
}
// A rising edge on the sync jack (pin SYNC_CLOCK on the Teensy).
EMU_EXPORT void emu_sync_edge(uint32_t now_us){ master.sync_edge(now_us); }
// main.cpp stirs the entropy pool at boot from things that differ between
// power cycles; the page does the same from its own randomness.
EMU_EXPORT void emu_entropy_stir(uint32_t value){ entropy::stir(value); }

// Master clock (#4). The page is the interval timer and the sync pin
// interrupt: it calls emu_clock_advance() once per subtick_interval_us() of
// simulated time, reprogramming itself when take_interval_change() says so,
// exactly as src/hal/teensy/teensy_clock.cpp does with the IntervalTimer.
EMU_EXPORT void emu_clock_advance(){ master.clock().advance(); }
EMU_EXPORT uint32_t emu_clock_interval_us(){ return master.clock().subtick_interval_us(); }
EMU_EXPORT uint32_t emu_clock_take_interval_change(){ return master.clock().take_interval_change() ? 1 : 0; }
EMU_EXPORT uint32_t emu_clock_count(){ return master.clock().count(); }
EMU_EXPORT uint32_t emu_clock_running(){ return master.clock().running() ? 1 : 0; }
EMU_EXPORT uint32_t emu_clock_source(){ return master.clock().source(); }
EMU_EXPORT void emu_clock_set_source(uint32_t source){ master.clock().set_source((uint8_t)source); }
EMU_EXPORT uint32_t emu_clock_bpm(){ return master.clock().bpm(); }
EMU_EXPORT void emu_clock_set_bpm(uint32_t bpm){ master.clock().set_bpm((uint16_t)bpm); }
EMU_EXPORT uint32_t emu_clock_cv_ppqn(){ return master.clock().cv_ppqn(); }
EMU_EXPORT void emu_clock_set_cv_ppqn(uint32_t ppqn){ master.clock().set_cv_ppqn((uint8_t)ppqn); }
EMU_EXPORT void emu_clock_start(){ master.clock().start(); }
EMU_EXPORT void emu_clock_stop(){ master.clock().stop(); }
EMU_EXPORT void emu_clock_resume(){ master.clock().resume(); }
EMU_EXPORT uint32_t emu_clock_rejected_edges(){ return master.clock().rejected_edges(); }

// Jacks -------------------------------------------------------------------

EMU_EXPORT void emu_jack_set_input(uint32_t port, uint32_t level){ if (port < GPIO_N) gpio.inputs[port] = level ? GPIO_HIGH : GPIO_LOW; }
EMU_EXPORT uint32_t emu_jack_input(uint32_t port){ return port < GPIO_N ? gpio.inputs[port] : 0; }
EMU_EXPORT uint32_t emu_jack_output(uint32_t port){ return port < GPIO_N ? gpio.outputs[port] : 0; }
EMU_EXPORT uint32_t emu_jack_mode(uint32_t port){ return port < GPIO_N ? gpio.modes[port] : 0; }

// Buses (the front buffer: what readers saw this pass) ----------------------

EMU_EXPORT uint32_t emu_gate_buses(){
    uint32_t mask = 0;
    for (uint8_t b = 0; b < N_GATE_BUS; b++) if (master.buses().gate_read(b)) mask |= (1u << b);
    return mask;
}
EMU_EXPORT uint32_t emu_note_count(uint32_t bus){ return master.buses().note_count((uint8_t)bus); }
EMU_EXPORT uint32_t emu_note_overflows(uint32_t bus){ return master.buses().note_overflows((uint8_t)bus); }
// One event of a note bus packed as type<<24 | channel<<16 | d1<<8 | d2.
EMU_EXPORT uint32_t emu_note_event(uint32_t bus, uint32_t index){
    const MidiEvent& e = master.buses().note_read((uint8_t)bus, (uint8_t)index);
    return ((uint32_t)e.type << 24) | ((uint32_t)e.channel << 16) | ((uint32_t)e.data1 << 8) | e.data2;
}
EMU_EXPORT int32_t emu_cv(uint32_t bus){ return master.buses().cv_read((uint8_t)bus); }
