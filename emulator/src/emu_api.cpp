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
#include "algorithm/sequencer/gate_sequencer.h"
#include "algorithm/sequencer/note_sequencer.h"
#include "algorithm/sequencer/drum_sequencer.h"
#include "midi/scale.h"
#include "patch/patch_manager.h"
#include "patch/patch_store.h"
#include "patch/default_patch.h"
#include "protocol/sysex_handler.h"
#include "control/cc_mapper.h"
#include "control/nrpn.h"
#include "led/status_leds.h"
#include "web_hal.h"

#define EMU_EXPORT extern "C" __attribute__((visibility("default")))

static WebGpio gpio;
static WebMidiOut midi;
static WebEeprom eeprom;
static WebLeds led_driver;
static MixedModeMaster master(gpio, midi);

// The control plane, exactly as main.cpp wires it (#7, #11, #21, #22). The
// page can therefore drive the module the way a host does - identity request,
// dump, incremental edit, CC, NRPN - against the firmware's own protocol
// implementation rather than a JavaScript imitation of it, which is what lets
// the editor's tests check themselves against the real validator (#12).
static StatusLeds leds(led_driver);
static PatchStore store(eeprom);
static PatchManager patches(master, store, leds);
static CcMapper cc_map(patches, master);
static NrpnDecoder nrpn(patches, cc_map);
static SysexHandler protocol(patches, master, store, leds, midi, cc_map);

// The patch under construction. Separate from what is running: the page fills
// this through the emu_patch_* setters and calls emu_load() to make it live.
static Patch patch = empty_patch();
static GlobalSettings globals = default_globals();

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
EMU_EXPORT uint32_t emu_const_note_seq_voices(){ return NOTE_SEQ_VOICES; }
EMU_EXPORT uint32_t emu_const_drum_seq_lanes(){ return DRUM_SEQ_LANES; }
EMU_EXPORT uint32_t emu_const_node_slot_size(){ return NODE_SLOT_SIZE; }
// The scales (midi/scale.h), as the 12-bit masks the sequencers and
// NoteQuantise store, so the page resolves a scale name to the firmware's
// mask. SCALE_GLOBAL is in the list and its mask is zero: that is what the
// firmware reads as "follow the module's scale".
EMU_EXPORT uint32_t emu_scale_count(){ return SCALE_COUNT; }
EMU_EXPORT uint32_t emu_scale_mask(uint32_t id){ return id < SCALE_COUNT ? scale_mask((uint8_t)id) : 0; }

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
    // Through PatchManager, so the store, the LEDs and everything the
    // protocol reports stay in step with what is running.
    if (patches.apply(patch, globals, 0) != APPLY_OK) return master.last_error();
    cc_map.reset();
    return LOAD_OK;
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

// Sequencer view (#13, #14) -----------------------------------------------
//
// What the page's step grids show: the pattern each loaded sequencer holds,
// the step each lane is on, and for the note sequencers the pitch every cell
// resolves to against the node's *current* root and scale, so playing a key
// into the root inlet visibly re-pitches the grid. Read-only, and resolved
// from the descriptor id rather than RTTI, which the wasm build does without.

enum SeqKind : uint32_t { SEQ_NONE = 0, SEQ_GATE = 1, SEQ_NOTE = 2, SEQ_DRUM = 3 };

static uint32_t seq_kind(uint32_t i, Node*& node){
    node = master.node((uint8_t)i);
    const AlgorithmDescriptor* d = master.node_descriptor((uint8_t)i);
    if (node == nullptr || d == nullptr) return SEQ_NONE;
    switch (d->id){
        case ALGO_METRONOME: case ALGO_STEP_SEQ: case ALGO_EUCLID_SEQ: case ALGO_RANDOM_SEQ: return SEQ_GATE;
        case ALGO_NOTE_SEQ: case ALGO_POLY_SEQ: return SEQ_NOTE;
        case ALGO_DRUM_SEQ_GATE: case ALGO_DRUM_SEQ_MIDI: return SEQ_DRUM;
        default: return SEQ_NONE;
    }
}

EMU_EXPORT uint32_t emu_seq_kind(uint32_t i){ Node* n; return seq_kind(i, n); }
// Lanes: 1 for a gate sequencer, the voices for a note sequencer, the lanes
// for a drum sequencer.
EMU_EXPORT uint32_t emu_seq_lanes(uint32_t i){
    Node* n;
    switch (seq_kind(i, n)){
        case SEQ_GATE: return 1;
        case SEQ_NOTE: return static_cast<NoteSequencerBase*>(n)->voices();
        case SEQ_DRUM: return DrumSequencer::LANES;
        default: return 0;
    }
}
EMU_EXPORT uint32_t emu_seq_length(uint32_t i, uint32_t lane){
    Node* n;
    switch (seq_kind(i, n)){
        case SEQ_GATE: return static_cast<GateSequencer*>(n)->length();
        case SEQ_NOTE: return static_cast<NoteSequencerBase*>(n)->length();
        case SEQ_DRUM: return static_cast<DrumSequencer*>(n)->lane_length((uint8_t)lane);
        default: return 0;
    }
}
// The step the lane is on, or 0xFF before its first advance.
EMU_EXPORT uint32_t emu_seq_position(uint32_t i, uint32_t lane){
    Node* n;
    switch (seq_kind(i, n)){
        case SEQ_GATE: { GateSequencer* g = static_cast<GateSequencer*>(n); return g->steps_taken() ? g->position() : 0xFF; }
        case SEQ_NOTE: { NoteSequencerBase* s = static_cast<NoteSequencerBase*>(n); return s->steps_taken() ? s->position() : 0xFF; }
        case SEQ_DRUM: { DrumSequencer* d = static_cast<DrumSequencer*>(n); return d->steps_taken() ? d->lane_position((uint8_t)lane) : 0xFF; }
        default: return 0xFF;
    }
}
// A cell: 1/0 for gate patterns, the stored velocity for note and MIDI drum
// cells (0 is silent).
EMU_EXPORT uint32_t emu_seq_cell(uint32_t i, uint32_t lane, uint32_t step){
    Node* n;
    switch (seq_kind(i, n)){
        case SEQ_GATE: return static_cast<GateSequencer*>(n)->on((uint8_t)step) ? 1 : 0;
        case SEQ_NOTE: return static_cast<NoteSequencerBase*>(n)->velocity((uint8_t)step, (uint8_t)lane);
        case SEQ_DRUM: return static_cast<DrumSequencer*>(n)->cell((uint8_t)lane, (uint8_t)step);
        default: return 0;
    }
}
// Note sequencers: the pitch a cell resolves to now (0xFF if silent or out
// of range), its degree, the step's length|flags byte, the current root.
EMU_EXPORT uint32_t emu_seq_pitch(uint32_t i, uint32_t lane, uint32_t step){
    Node* n;
    return seq_kind(i, n) == SEQ_NOTE ? static_cast<NoteSequencerBase*>(n)->pitch((uint8_t)step, (uint8_t)lane) : 0xFF;
}
EMU_EXPORT int32_t emu_seq_degree(uint32_t i, uint32_t lane, uint32_t step){
    Node* n;
    return seq_kind(i, n) == SEQ_NOTE ? static_cast<NoteSequencerBase*>(n)->degree((uint8_t)step, (uint8_t)lane) : 0;
}
EMU_EXPORT uint32_t emu_seq_flags(uint32_t i, uint32_t step){
    Node* n;
    return seq_kind(i, n) == SEQ_NOTE ? static_cast<NoteSequencerBase*>(n)->flags((uint8_t)step) : 0;
}
EMU_EXPORT uint32_t emu_seq_root(uint32_t i){
    Node* n;
    return seq_kind(i, n) == SEQ_NOTE ? static_cast<NoteSequencerBase*>(n)->root_note() : 0;
}
EMU_EXPORT uint32_t emu_seq_scale(uint32_t i){
    Node* n;
    return seq_kind(i, n) == SEQ_NOTE ? static_cast<NoteSequencerBase*>(n)->scale() : 0;
}
// Per-step (gate, note) or per-lane (drum) probability, percent.
EMU_EXPORT uint32_t emu_seq_probability(uint32_t i, uint32_t lane, uint32_t step){
    Node* n;
    switch (seq_kind(i, n)){
        case SEQ_GATE: return static_cast<GateSequencer*>(n)->probability((uint8_t)step);
        case SEQ_NOTE: return static_cast<NoteSequencerBase*>(n)->probability((uint8_t)step);
        case SEQ_DRUM: return static_cast<DrumSequencer*>(n)->lane_probability((uint8_t)lane);
        default: return 0;
    }
}
// Drum MIDI lanes: note number and channel (0 for the gate variant).
EMU_EXPORT uint32_t emu_seq_lane_note(uint32_t i, uint32_t lane){
    const AlgorithmDescriptor* d = master.node_descriptor((uint8_t)i);
    if (d == nullptr || d->id != ALGO_DRUM_SEQ_MIDI) return 0;
    return static_cast<DrumSeqMidi*>(master.node((uint8_t)i))->lane_note((uint8_t)lane);
}
EMU_EXPORT uint32_t emu_seq_lane_channel(uint32_t i, uint32_t lane){
    const AlgorithmDescriptor* d = master.node_descriptor((uint8_t)i);
    if (d == nullptr || d->id != ALGO_DRUM_SEQ_MIDI) return 0;
    return static_cast<DrumSeqMidi*>(master.node((uint8_t)i))->lane_channel((uint8_t)lane);
}
// The algorithm id of a loaded node, for the page's labels.
EMU_EXPORT uint32_t emu_node_algo(uint32_t i){
    const AlgorithmDescriptor* d = master.node_descriptor((uint8_t)i);
    return d ? d->id : 0;
}


// The control plane (#7, #11, #21, #22) -----------------------------------
//
// The page gets the firmware's own protocol handler rather than a JavaScript
// reimplementation, so an editor tested against the emulator is tested
// against the validator that will reject or accept its patches on hardware.

// Boot as the module does: slot 0 if it checks out, the flash default if not.
EMU_EXPORT uint32_t emu_boot(uint32_t now_us){
    patches.boot(now_us);
    cc_map.reset();
    patch = patches.active();
    globals = patches.globals();
    return patches.running_defaults() ? 1 : 0;
}

// Where the page writes a message before handing it over. Exported rather
// than left to the caller to find a free address: guessing at one happens to
// work until the linker moves something.
static uint8_t sysex_in_buffer[SYSEX_RX_MAX];
EMU_EXPORT uint8_t* emu_sysex_in_ptr(){ return sysex_in_buffer; }
EMU_EXPORT uint32_t emu_sysex_in_capacity(){ return SYSEX_RX_MAX; }

// Feeds one complete SysEx message, F0 to F7 inclusive, from `source`, at
// the page's simulated time - the same clock the passes run on.
EMU_EXPORT void emu_sysex_in(uint32_t source, const uint8_t* data, uint32_t length, uint32_t now_us){
    protocol.deliver_sysex((uint8_t)source, data, (uint16_t)length, now_us);
}
// Replies, concatenated in the order they were sent. The page reads them out
// and clears the buffer.
EMU_EXPORT const uint8_t* emu_sysex_out_ptr(){ return midi.sysex_bytes; }
EMU_EXPORT uint32_t emu_sysex_out_len(){ return midi.sysex_used; }
EMU_EXPORT uint32_t emu_sysex_out_dropped(){ return midi.sysex_dropped; }
EMU_EXPORT void emu_sysex_out_clear(){ midi.drain_sysex(); }

// One incoming CC through the control path, as main.cpp's loop offers it:
// NRPN first where it is enabled, then the mapping table. Returns 1 when the
// event was consumed and must not reach a note bus.
EMU_EXPORT uint32_t emu_control_cc(uint32_t source, uint32_t channel, uint32_t cc,
                                  uint32_t value, uint32_t now_us){
    if (nrpn.observe((uint8_t)source, (uint8_t)channel, (uint8_t)cc, (uint8_t)value, now_us)) return 1;
    if (cc_map.observe((uint8_t)source, (uint8_t)channel, (uint8_t)cc, (uint8_t)value, now_us)) return 1;
    return 0;
}
// A Program Change offered to preset recall; 1 when it was consumed.
EMU_EXPORT uint32_t emu_control_program_change(uint32_t source, uint32_t channel,
                                              uint32_t program, uint32_t now_us){
    return protocol.program_change((uint8_t)source, (uint8_t)channel, (uint8_t)program, now_us) ? 1 : 0;
}
// The last beat the green LED flashed on, so a flash happens once per beat
// rather than once per subtick (main.cpp).
static uint32_t last_beat = 0;
static bool have_beat = false;

// Once per loop, before the pass: applies whatever the controllers moved,
// services the protocol's timeouts and any pending quantised swap, and drives
// the feedback surface - the same five calls main.cpp's loop makes, in the
// same order. The beat belongs here rather than in the page: the LED
// vocabulary is the firmware's (led/status_leds.h), and a page that decided
// for itself when to flash would be showing something the module does not do.
EMU_EXPORT void emu_control_service(uint32_t now_us){
    cc_map.apply(now_us);
    protocol.service(now_us);
    nrpn.service(now_us);
    patches.service(now_us);
    const uint32_t beat = master.clock().count() / CLOCK_SUBTICKS_PER_QUARTER;
    if (!have_beat || beat != last_beat){
        if (have_beat) leds.beat(now_us);
        last_beat = beat;
        have_beat = true;
    }
    leds.set_clock_running(master.clock().running());
    leds.service(now_us);
}

EMU_EXPORT uint32_t emu_led(uint32_t which){ return which < LED_COUNT ? led_driver.levels[which] : 0; }
EMU_EXPORT uint32_t emu_slot_used(uint32_t slot){ return store.used((uint8_t)slot); }
EMU_EXPORT uint32_t emu_slot_occupied(uint32_t slot){ return store.occupied((uint8_t)slot) ? 1 : 0; }
EMU_EXPORT uint32_t emu_running_defaults(){ return patches.running_defaults() ? 1 : 0; }
EMU_EXPORT uint32_t emu_const_patch_slots(){ return PATCH_SLOTS; }
EMU_EXPORT uint32_t emu_const_patch_slot_bytes(){ return PATCH_SLOT_BYTES; }
EMU_EXPORT uint32_t emu_const_n_cc_map(){ return N_CC_MAP; }
EMU_EXPORT uint32_t emu_const_control_port(){ return MIDI_CONTROL_PORT; }
EMU_EXPORT uint32_t emu_const_sysex_manufacturer(){ return SYSEX_MANUFACTURER; }
EMU_EXPORT uint32_t emu_const_protocol_version(){ return SYSEX_PROTOCOL_VERSION; }
EMU_EXPORT uint32_t emu_const_chunk_payload(){ return SYSEX_CHUNK_PAYLOAD; }
EMU_EXPORT uint32_t emu_const_patch_format_version(){ return PATCH_FORMAT_VERSION; }

// Parameter descriptors (#20), so the page can draw a control for a
// parameter without hardcoding a table that drifts.
EMU_EXPORT uint32_t emu_algo_n_param_groups(uint32_t i){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    return d ? d->n_param_groups : 0;
}
EMU_EXPORT uint32_t emu_param_group_first(uint32_t i, uint32_t g){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    return (d && g < d->n_param_groups) ? d->param_groups[g].first : 0;
}
EMU_EXPORT uint32_t emu_param_group_repeat(uint32_t i, uint32_t g){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    return (d && g < d->n_param_groups) ? d->param_groups[g].repeat : 0;
}
EMU_EXPORT uint32_t emu_param_group_fields(uint32_t i, uint32_t g){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    return (d && g < d->n_param_groups) ? d->param_groups[g].n_fields : 0;
}
static const ParamDescriptor* group_field(uint32_t i, uint32_t g, uint32_t f){
    const AlgorithmDescriptor* d = registry::at((uint8_t)i);
    if (d == nullptr || g >= d->n_param_groups) return nullptr;
    const ParamGroup& grp = d->param_groups[g];
    return f < grp.n_fields ? &grp.fields[f] : nullptr;
}
EMU_EXPORT const char* emu_param_name(uint32_t i, uint32_t g, uint32_t f){
    const ParamDescriptor* p = group_field(i, g, f);
    return p ? p->name : "";
}
EMU_EXPORT uint32_t emu_param_min(uint32_t i, uint32_t g, uint32_t f){
    const ParamDescriptor* p = group_field(i, g, f); return p ? p->min : 0;
}
EMU_EXPORT uint32_t emu_param_max(uint32_t i, uint32_t g, uint32_t f){
    const ParamDescriptor* p = group_field(i, g, f); return p ? p->max : 0;
}
EMU_EXPORT uint32_t emu_param_default(uint32_t i, uint32_t g, uint32_t f){
    const ParamDescriptor* p = group_field(i, g, f); return p ? p->def : 0;
}
EMU_EXPORT uint32_t emu_param_kind(uint32_t i, uint32_t g, uint32_t f){
    const ParamDescriptor* p = group_field(i, g, f); return p ? p->kind : 0;
}
EMU_EXPORT const char* emu_param_option(uint32_t i, uint32_t g, uint32_t f, uint32_t o){
    const ParamDescriptor* p = group_field(i, g, f);
    if (p == nullptr || p->kind != PARAM_ENUM || p->options == nullptr) return "";
    return (o <= (uint32_t)(p->max - p->min)) ? p->options[o] : "";
}
