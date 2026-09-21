#include "algorithm/switch/note_switch.h"
#include "node/registry.h"
#include "midi/note_event.h"

// The pass-through every switch does for one event: a note-on is recorded
// so its note-off can be found later, a note-off is looked up rather than
// forwarded, and anything else goes through as it is.
static void pass_event(BusManager& bus, BusSet out, SoundingNotes& sounding,
                       uint8_t channel, const MidiEvent& e){
    if (is_note_off(e)){
        sounding.release(bus, out, e.data1);
        return;
    }
    if (is_note_on(e)){
        sounding.emit(bus, out, e.data1, e.data1, e.data2, out_channel(channel, e.channel));
        return;
    }
    bus.note_write(out, readdressed(e, channel));
}

// --- NoteSwitch --------------------------------------------------------------

static const Domain SWITCH_IN[MAX_IN] = {
    Domain::Note, Domain::Note, Domain::Note, Domain::Note, Domain::Note,
    Domain::CV, Domain::Gate, Domain::Gate };
static_assert(NoteSwitch::POSITIONS == 5, "SWITCH_IN lists five parts, then select, step and reset");
static const Domain SWITCH_OUT[1] = {Domain::Note};

static const char* const SWITCH_IN_NAMES[MAX_IN] = {
    "in 1", "in 2", "in 3", "in 4", "in 5", "select", "step", "reset"};
static const char* const SWITCH_OUT_NAMES[1] = {"out"};

static const ParamDescriptor SWITCH_PARAMS[NoteSwitch::N_PARAMS] = {
    {"select",  1, NoteSwitch::POSITIONS, 1, PARAM_NUMBER,      nullptr},
    {"steps",   0, NoteSwitch::POSITIONS, 0, PARAM_ENUM,        SWITCH_STEPS_NAMES},
    {"channel", 0, 16, 0,                    PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup SWITCH_GROUPS[1] = {{0, 1, NoteSwitch::N_PARAMS, SWITCH_PARAMS}};

const AlgorithmDescriptor NoteSwitch::descriptor = {
    ALGO_NOTE_SWITCH, "NoteSwitch", MAX_IN, 0, 1, NoteSwitch::N_PARAMS, SWITCH_IN, SWITCH_OUT,
    sizeof(NoteSwitch), false, construct_node<NoteSwitch>,
    SWITCH_GROUPS, 1, SWITCH_IN_NAMES, SWITCH_OUT_NAMES,
    "Many parts to one: the outlet plays the selected inlet. Switching releases what it leaves.",
    CATEGORY_MIDI };

NoteSwitch::NoteSwitch(const NodeConfig& config) :
    in(),
    out(config.out_buses[0]),
    sel(config.in_buses[IN_SELECT], config.in_buses[IN_STEP], config.in_buses[IN_RESET],
        POSITIONS, ports_patched(config.in_buses, POSITIONS),
        config.params[P_SELECT], config.params[P_STEPS]),
    sounding(),
    playing(sel.position()),
    channel(config.params[P_CHANNEL] <= 16 ? config.params[P_CHANNEL] : (uint8_t)CHANNEL_FROM_SOURCE)
{
    for (uint8_t i = 0; i < POSITIONS; i++) in[i] = config.in_buses[i];
}

void NoteSwitch::process(BusManager& bus, uint32_t){
    // Release before reading the new part, so the note-offs of the old one
    // land ahead of the first note-on of the new one on the same bus. The
    // comparison is against the part the ledger came from rather than
    // against last pass, because a parameter write moves the switch between
    // passes and owes the same release.
    sel.update(bus);
    if (sel.position() != playing){
        sounding.release_all(bus, out);
        playing = sel.position();
    }
    const BusSet chosen = in[playing];
    if (!chosen.any()) return;
    const uint8_t n = bus.note_count(chosen);
    for (uint8_t i = 0; i < n; i++) pass_event(bus, out, sounding, channel, bus.note_read(chosen, i));
}

void NoteSwitch::silence(BusManager& bus){
    sounding.release_all(bus, out);
}

// A parameter write is a switch like any other and releases like one, on
// the next process(): a release needs the bus, and set_param has not got one.
bool NoteSwitch::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_SELECT:  return sel.set_select(value);
        case P_STEPS:   return sel.set_steps(value);
        case P_CHANNEL: if (value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t NoteSwitch::get_param(uint16_t index) const {
    switch (index){
        case P_SELECT:  return sel.select();
        case P_STEPS:   return sel.steps();
        case P_CHANNEL: return channel;
        default: return 0;
    }
}

// --- NoteRouter --------------------------------------------------------------

static const Domain ROUTER_IN[4] = {Domain::Note, Domain::CV, Domain::Gate, Domain::Gate};
static const Domain ROUTER_OUT[MAX_OUT] = {
    Domain::Note, Domain::Note, Domain::Note, Domain::Note,
    Domain::Note, Domain::Note, Domain::Note, Domain::Note };
static_assert(NoteRouter::POSITIONS == 8, "ROUTER_OUT lists one domain per outlet");

static const char* const ROUTER_IN_NAMES[4] = {"in", "select", "step", "reset"};
static const char* const ROUTER_OUT_NAMES[MAX_OUT] = {
    "out 1", "out 2", "out 3", "out 4", "out 5", "out 6", "out 7", "out 8"};

static const ParamDescriptor ROUTER_PARAMS[NoteRouter::N_PARAMS] = {
    {"select",  1, NoteRouter::POSITIONS, 1, PARAM_NUMBER,      nullptr},
    {"steps",   0, NoteRouter::POSITIONS, 0, PARAM_ENUM,        SWITCH_STEPS_NAMES},
    {"channel", 0, 16, 0,                    PARAM_CHANNEL_OUT, nullptr},
};
static const ParamGroup ROUTER_GROUPS[1] = {{0, 1, NoteRouter::N_PARAMS, ROUTER_PARAMS}};

const AlgorithmDescriptor NoteRouter::descriptor = {
    ALGO_NOTE_ROUTER, "NoteRouter", 4, 1, MAX_OUT, NoteRouter::N_PARAMS, ROUTER_IN, ROUTER_OUT,
    sizeof(NoteRouter), false, construct_node<NoteRouter>,
    ROUTER_GROUPS, 1, ROUTER_IN_NAMES, ROUTER_OUT_NAMES,
    "One part to many: the inlet plays out of the selected outlet. Switching releases what it leaves.",
    CATEGORY_MIDI };

NoteRouter::NoteRouter(const NodeConfig& config) :
    in(config.in_buses[IN_SIGNAL]),
    out(),
    sel(config.in_buses[IN_SELECT], config.in_buses[IN_STEP], config.in_buses[IN_RESET],
        POSITIONS, ports_patched(config.out_buses, POSITIONS),
        config.params[P_SELECT], config.params[P_STEPS]),
    sounding(),
    sounding_on(sel.position()),
    channel(config.params[P_CHANNEL] <= 16 ? config.params[P_CHANNEL] : (uint8_t)CHANNEL_FROM_SOURCE)
{
    for (uint8_t i = 0; i < POSITIONS; i++) out[i] = config.out_buses[i];
}

void NoteRouter::process(BusManager& bus, uint32_t){
    sel.update(bus);
    if (sel.position() != sounding_on){
        sounding.release_all(bus, out[sounding_on]);
        sounding_on = sel.position();
    }
    const BusSet chosen = out[sounding_on];
    const uint8_t n = bus.note_count(in);
    for (uint8_t i = 0; i < n; i++) pass_event(bus, chosen, sounding, channel, bus.note_read(in, i));
}

void NoteRouter::silence(BusManager& bus){
    sounding.release_all(bus, out[sounding_on]);
}

bool NoteRouter::set_param(uint16_t index, uint8_t value){
    switch (index){
        case P_SELECT:  return sel.set_select(value);
        case P_STEPS:   return sel.set_steps(value);
        case P_CHANNEL: if (value > 16) return false; channel = value; return true;
        default: return false;
    }
}

uint8_t NoteRouter::get_param(uint16_t index) const {
    switch (index){
        case P_SELECT:  return sel.select();
        case P_STEPS:   return sel.steps();
        case P_CHANNEL: return channel;
        default: return 0;
    }
}
