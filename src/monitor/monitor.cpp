#include "monitor/monitor.h"
#include "node/registry.h"
#include "algorithm/sequencer/gate_sequencer.h"
#include "algorithm/sequencer/note_sequencer.h"
#include "algorithm/sequencer/drum_sequencer.h"
#include "algorithm/midi/harmony.h"

static_assert(MONITOR_NODE_VALUES >= DRUM_SEQ_LANES,
              "a drum sequencer's lanes must fit one node's positions");
static_assert(MONITOR_NODE_VALUES >= 2 + Harmony::MAX_PHRASE,
              "a harmony's degree, loop slot and loop must fit one node's positions");
static_assert(MONITOR_EVENTS < 0x80, "the event count travels as one data byte");

Monitor::Monitor(const MixedModeMaster& master, HalTap& panel, const StatusLeds& status) :
    mm(master), tap(panel), leds(status),
    note_mask(0), watched_at(0), watching(false),
    at(0), g_now(0), g_since(0), ji_now(0), ji_since(0), jo_now(0), jo_since(0),
    led_hi(), cv_last(), events(), n_events(0), lost(false)
{}

void Monitor::watch(uint16_t note_buses, uint32_t now_us){
    if (!armed(now_us)) clear(now_us);
    note_mask = note_buses;
    watched_at = now_us;
    watching = true;
}

void Monitor::clear(uint32_t now_us){
    at = now_us;
    g_now = g_since = 0;
    ji_now = ji_since = jo_now = jo_since = 0;
    for (uint8_t i = 0; i < LED_COUNT; i++) led_hi[i] = 0;
    for (uint8_t b = 0; b < N_CV_BUS; b++) cv_last[b] = 0;
    n_events = 0;
    lost = false;
}

void Monitor::take(){
    g_since = g_now;
    ji_since = ji_now;
    jo_since = jo_now;
    for (uint8_t i = 0; i < LED_COUNT; i++) led_hi[i] = leds.level(i);
    n_events = 0;
    lost = false;
}

void Monitor::record(uint32_t now_us, uint8_t where, uint8_t arg, const MidiEvent& e){
    if (e.type != MIDI_NOTE_ON && e.type != MIDI_NOTE_OFF) return;
    if (n_events >= MONITOR_EVENTS){ lost = true; return; }
    events[n_events++] = Event{now_us, where, arg, e.type, e.channel, e.data1, e.data2};
}

void Monitor::sample(uint32_t now_us){
    if (!armed(now_us)){
        // Disarmed: the tap still collected this pass's notes, and they are
        // nobody's. Cleared here so a pass costs the same whether or not it
        // was watched, and so arming later starts from an empty record.
        tap.clear_sent();
        watching = false;
        return;
    }
    at = now_us;

    const BusManager& bus = mm.buses();
    uint32_t gates = 0;
    for (uint8_t b = 0; b < N_GATE_BUS; b++) if (bus.gate_read(b)) gates |= (1u << b);
    g_now = gates;
    g_since |= gates;

    ji_now = tap.jacks_in();
    jo_now = tap.jacks_out();
    ji_since |= ji_now;
    jo_since |= jo_now;

    for (uint8_t i = 0; i < LED_COUNT; i++){
        const uint8_t level = leds.level(i);
        if (level > led_hi[i]) led_hi[i] = level;
    }
    for (uint8_t b = 0; b < N_CV_BUS; b++) cv_last[b] = bus.cv_read(b);

    // The note buses somebody is watching, drained for this pass: the
    // buses are double-buffered and the pass swapped once, so the front
    // buffer holds exactly what this pass wrote - every event once.
    for (uint8_t b = 0; b < N_NOTE_BUS; b++){
        if ((note_mask & (1u << b)) == 0) continue;
        const uint8_t n = bus.note_count(b);
        for (uint8_t i = 0; i < n; i++) record(now_us, WHERE_BUS, b, bus.note_read(b, i));
    }
    // And what left the module this pass, off the tap.
    const uint16_t sent = tap.sent_count();
    for (uint16_t i = 0; i < sent; i++){
        const HalTap::Sent& s = tap.sent_at(i);
        const MidiEvent e = {s.type, s.channel, s.d1, s.d2};
        record(now_us, WHERE_OUT, s.target, e);
    }
    if (tap.sent_overflowed()) lost = true;
    tap.clear_sent();
}

uint8_t Monitor::positions(uint8_t node, uint8_t* out, uint8_t capacity) const {
    const Node* n = mm.node(node);
    const AlgorithmDescriptor* d = mm.node_descriptor(node);
    if (n == nullptr || d == nullptr || capacity == 0) return 0;
    uint8_t count = 0;
    const auto put = [&](uint8_t value){ if (count < capacity) out[count++] = value; };
    switch (d->id){
        case ALGO_STEP_SEQ: case ALGO_EUCLID_SEQ: case ALGO_RANDOM_SEQ: {
            const GateSequencer* g = static_cast<const GateSequencer*>(n);
            put(g->steps_taken() ? g->position() : NONE);
            return count;
        }
        case ALGO_NOTE_SEQ: case ALGO_POLY_SEQ: {
            const NoteSequencerBase* s = static_cast<const NoteSequencerBase*>(n);
            // One step for every voice: the lanes advance together, and a
            // display draws a playhead per lane.
            for (uint8_t v = 0; v < s->voices(); v++) put(s->steps_taken() ? s->position() : NONE);
            return count;
        }
        case ALGO_DRUM_SEQ_GATE: case ALGO_DRUM_SEQ_MIDI: {
            const DrumSequencer* ds = static_cast<const DrumSequencer*>(n);
            for (uint8_t lane = 0; lane < DrumSequencer::LANES; lane++){
                put(ds->steps_taken() ? ds->lane_position(lane) : NONE);
            }
            return count;
        }
        case ALGO_HARMONY: {
            const Harmony* h = static_cast<const Harmony*>(n);
            put(h->degree());
            put(h->loop_position());
            const uint8_t loop = h->get_param(Harmony::P_LOOP);
            for (uint8_t slot = 0; slot < loop; slot++) put(h->loop_chord(slot));
            return count;
        }
        default:
            return 0;
    }
}
