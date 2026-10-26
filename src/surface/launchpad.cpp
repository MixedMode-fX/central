#include "surface/launchpad.h"
#include "midi/global_key.h"
#include "midi/scale.h"
#include "protocol/sysex.h"

// The device inquiry, to any device: a Launchpad answers with its family,
// and the answer is what links it (deliver_sysex).
static const uint8_t INQUIRY[] = {0xF0, SYSEX_UNIVERSAL_NON_REALTIME, SYSEX_BROADCAST_DEVICE,
                                  SYSEX_GENERAL_INFORMATION, SYSEX_IDENTITY_REQUEST, 0xF7};

Launchpad::Launchpad(PatchManager& manager, MixedModeMaster& master, CcMapper& mapper, IMidiOut& out,
                     IReverb& reverb) :
    patches(manager), mm(master), cc(mapper), midi(out), fx(reverb),
    active_port(0), active_model(LP_MODEL_AUTO), seen_model(LP_MODEL_AUTO), is_linked(false),
    last_inquiry_us(0), last_seen_us(0), ever_asked(false),
    current_page(PAGE_NOTE), before_reverb(PAGE_NOTE), clear_down(0), shift(0), widgets_page(0),
    held_note(), held_channel(), fader_value(), toggles_on(), buttons_down(), side_down(0),
    want(), shown(), last_led_us(0), tx()
{
    for (uint8_t i = 0; i < PADS; i++){ held_note[i] = NO_NOTE; held_channel[i] = 0; }
    invalidate();
}

uint8_t Launchpad::dim(uint8_t colour){
    if (colour < 4 || colour > 59) return DIM;
    return (uint8_t)(4u + ((colour - 4u) / 4u) * 4u + 2u);
}

uint16_t Launchpad::bar_tempo(uint8_t step){
    if (step > 15) step = 15;
    return (uint16_t)(CLOCK_MIN_BPM + ((uint32_t)step * (CLOCK_MAX_BPM - CLOCK_MIN_BPM) + 7u) / 15u);
}

uint8_t Launchpad::model() const {
    if (seen_model != LP_MODEL_AUTO) return seen_model;
    return config().model;
}

uint8_t Launchpad::device_byte() const {
    switch (model()){
        case LP_MODEL_X:       return DEVICE_X;
        case LP_MODEL_PRO_MK3: return DEVICE_PRO_MK3;
        default:               return DEVICE_MINI_MK3;
    }
}

// A Mini's or an X's top row is four arrows and four buttons nothing else
// claims. A Pro's starts left, right, Session, Note, Chord, Custom, and its up
// and down head the left column, so it gets a layout of its own: its printed
// arrows are the arrows, and the pages sit on Session to Custom - the note
// page on Note.
uint8_t Launchpad::button_id(uint8_t button) const {
    static const uint8_t PRO[TOP_BUTTONS] = {80, 70, 91, 92, 93, 94, 95, 96};
    if (button >= TOP_BUTTONS) return 0;
    return model() == LP_MODEL_PRO_MK3 ? PRO[button] : top_id(button);
}

bool Launchpad::is_din() const {
    return (active_port & (mmMIDI_SERIAL_1 | mmMIDI_SERIAL_2)) != 0;
}

uint32_t Launchpad::led_interval() const {
    return is_din() ? LED_INTERVAL_DIN_US : LED_INTERVAL_USB_US;
}

// The link ------------------------------------------------------------------

void Launchpad::follow_config(uint32_t now_us){
    const LaunchpadConfig& c = config();
    if (c.port == active_port && c.model == active_model) return;
    // Leaving a cable: whatever was on it goes back to being a Launchpad,
    // with nothing of ours held down or lit.
    unlink(now_us);
    active_port = c.port;
    active_model = c.model;
    seen_model = LP_MODEL_AUTO;
    ever_asked = false;
    last_inquiry_us = now_us;
}

void Launchpad::unlink(uint32_t now_us){
    release_all(now_us);
    if (is_linked && active_port != 0){
        leave_programmer_mode();
    }
    is_linked = false;
    invalidate();
}

void Launchpad::invalidate(){
    for (uint8_t i = 0; i < LEDS; i++) shown[i] = UNKNOWN;
}

void Launchpad::enter_programmer_mode(){
    const uint8_t m[] = {0xF0, NOVATION_0, NOVATION_1, NOVATION_2, 0x02, device_byte(), 0x0E, 0x01, 0xF7};
    midi.send_sysex(active_port, m, (uint16_t)sizeof m);
}

void Launchpad::leave_programmer_mode(){
    const uint8_t m[] = {0xF0, NOVATION_0, NOVATION_1, NOVATION_2, 0x02, device_byte(), 0x0E, 0x00, 0xF7};
    midi.send_sysex(active_port, m, (uint16_t)sizeof m);
}

void Launchpad::send_inquiry(uint32_t now_us){
    last_inquiry_us = now_us;
    ever_asked = true;
    midi.send_sysex(active_port, INQUIRY, (uint16_t)sizeof INQUIRY);
}

// A model's layout readback: F0 00 20 29 02 <dev> 00 F7. Only the model
// whose id it carries answers, and its answer carries that id back.
void Launchpad::ask_model(uint8_t device){
    const uint8_t m[] = {0xF0, NOVATION_0, NOVATION_1, NOVATION_2, 0x02, device, COMMAND_LAYOUT, 0xF7};
    midi.send_sysex(active_port, m, (uint16_t)sizeof m);
}

uint8_t Launchpad::model_of(uint8_t device){
    switch (device){
        case DEVICE_MINI_MK3: return LP_MODEL_MINI_MK3;
        case DEVICE_X:        return LP_MODEL_X;
        case DEVICE_PRO_MK3:  return LP_MODEL_PRO_MK3;
        default:              return LP_MODEL_AUTO;
    }
}

void Launchpad::link(uint32_t now_us){
    is_linked = true;
    enter_programmer_mode();
    invalidate();
    last_led_us = now_us - led_interval();   // the first frame goes at once
}

void Launchpad::deliver_sysex(uint8_t source, const uint8_t* data, uint16_t length, uint32_t now_us){
    if (active_port == 0 || source != active_port) return;
    if (data == nullptr || length < 7 || data[0] != 0xF0) return;

    // The device's own message: F0 00 20 29 02 <dev> ... names the model.
    if (data[1] == NOVATION_0 && data[2] == NOVATION_1 && data[3] == NOVATION_2 && data[4] == 0x02){
        const uint8_t heard = model_of(data[5]);
        if (heard == LP_MODEL_AUTO) return;
        last_seen_us = now_us;
        if (heard == seen_model && is_linked) return;
        seen_model = heard;
        link(now_us);                         // again, if it was linked as another model
        return;
    }

    // F0 7E <dev> 06 02 00 20 29 <family lo hi> <member lo hi> <version x4> F7
    if (length < 12 || data[1] != SYSEX_UNIVERSAL_NON_REALTIME) return;
    if (data[3] != SYSEX_GENERAL_INFORMATION || data[4] != SYSEX_IDENTITY_REPLY) return;
    if (data[5] != NOVATION_0 || data[6] != NOVATION_1 || data[7] != NOVATION_2) return;
    if (config().model != LP_MODEL_AUTO){
        last_seen_us = now_us;
        if (!is_linked) link(now_us);
        return;
    }
    // Detected, the heartbeat is the model's own answer rather than this
    // one, which another model gives too: a Pro put on the cable in a Mini's
    // place answers the inquiry but not the Mini's readback, so the link
    // lapses and the next answer finds the Pro.
    if (seen_model != LP_MODEL_AUTO){ ask_model(device_byte()); return; }
    ask_model(DEVICE_MINI_MK3);
    ask_model(DEVICE_X);
    ask_model(DEVICE_PRO_MK3);
}

// Input ----------------------------------------------------------------------

bool Launchpad::receive(uint8_t source, const MidiEvent& e, uint32_t now_us){
    if (active_port == 0 || source != active_port) return false;
    // Everything on the surface's cable is the surface's, linked or not: a
    // Launchpad still in a factory mode plays nothing into the patch.
    if (!is_linked) return true;
    const uint8_t type = (uint8_t)(e.type & 0xF0);
    if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF){
        const uint8_t row = (uint8_t)(e.data1 / 10u), col = (uint8_t)(e.data1 % 10u);
        if (row >= 1 && row <= GRID && col >= 1 && col <= GRID){
            const bool down = type == MIDI_NOTE_ON && e.data2 > 0;
            pad((uint8_t)(row - 1u), (uint8_t)(col - 1u), e.data2, down, now_us);
        }
        return true;
    }
    if (type == MIDI_CONTROL_CHANGE){
        const bool down = e.data2 >= 64;
        for (uint8_t b = 0; b < TOP_BUTTONS; b++){
            if (e.data1 == button_id(b)){ top(b, down, now_us); return true; }
        }
        if (e.data1 % 10u == 9 && e.data1 >= 19 && e.data1 <= 89){
            side((uint8_t)(e.data1 / 10u - 1u), down, now_us);
            return true;
        }
        return true;
    }
    if (type == MIDI_AFTERTOUCH_POLY){
        // Pressure on a pad of the note page is pressure on the note it is
        // holding, which is the only note it could mean.
        const uint8_t row = (uint8_t)(e.data1 / 10u), col = (uint8_t)(e.data1 % 10u);
        if (current_page == PAGE_NOTE && row >= 1 && row <= GRID && col >= 1 && col <= GRID){
            const uint8_t i = (uint8_t)((row - 1u) * GRID + (col - 1u));
            if (held_note[i] != NO_NOTE){
                const MidiEvent at = {MIDI_AFTERTOUCH_POLY, held_channel[i], held_note[i], e.data2};
                mm.deliver_midi(active_port, at, now_us);
            }
        }
        return true;
    }
    if (type == MIDI_AFTERTOUCH_CHANNEL && current_page == PAGE_NOTE){
        const LaunchpadConfig& c = config();
        const MidiEvent at = {MIDI_AFTERTOUCH_CHANNEL, (uint8_t)(c.channel ? c.channel : 1u), e.data1, 0};
        mm.deliver_midi(active_port, at, now_us);
        return true;
    }
    return true;
}

void Launchpad::pad(uint8_t row, uint8_t col, uint8_t velocity, bool down, uint32_t now_us){
    switch (current_page){
        case PAGE_NOTE:    note_pad(row, col, velocity, down, now_us); break;
        case PAGE_KEY:     if (down) key_pad(row, col, now_us); break;
        case PAGE_WIDGETS: widget_pad(row, col, down, now_us); break;
        case PAGE_TEMPO:   if (down) tempo_pad(row, col, now_us); break;
        case PAGE_REVERB:  reverb_pad(row, col, down); break;
        default: break;
    }
}

void Launchpad::top(uint8_t index, bool down, uint32_t now_us){
    if (!down) return;
    switch (index){
        case TOP_KEY:     set_page(PAGE_KEY, now_us); return;
        case TOP_NOTE:    set_page(PAGE_NOTE, now_us); return;
        case TOP_WIDGETS: set_page(PAGE_WIDGETS, now_us); return;
        case TOP_TEMPO:   set_page(PAGE_TEMPO, now_us); return;
        default: break;
    }
    const int8_t dir = (index == TOP_UP || index == TOP_RIGHT) ? 1 : -1;
    const bool vertical = index == TOP_UP || index == TOP_DOWN;
    switch (current_page){
        case PAGE_NOTE:
            if (!vertical) return;
            // The register moves under nothing: a held note keeps the pitch
            // it was played at, and the next press plays the new one.
            if (shift + dir > OCTAVE_SHIFT_MAX || shift + dir < -OCTAVE_SHIFT_MAX) return;
            shift = (int8_t)(shift + dir);
            return;
        case PAGE_KEY: {
            if (!vertical) return;
            const int16_t octave = (int16_t)global_key::octave() + dir;
            if (octave < 1 || octave > KEY_MAX_OCTAVE) return;
            cc.write_control(CC_TARGET_KEY, 0, CC_KEY_OCTAVE, (uint16_t)octave, now_us);
            return;
        }
        case PAGE_WIDGETS:
            if (vertical) return;
            if ((int16_t)widgets_page + dir < 0 || (int16_t)widgets_page + dir >= WIDGET_PAGES) return;
            widgets_page = (uint8_t)(widgets_page + dir);
            return;
        case PAGE_TEMPO:
            nudge_tempo((int16_t)(vertical ? dir : dir * 10), now_us);
            return;
        default: return;
    }
}

void Launchpad::side(uint8_t index, bool down, uint32_t now_us){
    if (down) side_down |= (uint8_t)(1u << index); else side_down &= (uint8_t)~(1u << index);
    if (!down) return;
    switch (index){
        case SIDE_PLAY:
            cc.write_control(CC_TARGET_TRANSPORT, 0,
                             mm.clock().running() ? CC_TRANSPORT_STOP : CC_TRANSPORT_START, 1, now_us);
            return;
        case SIDE_CONTINUE: cc.write_control(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_CONTINUE, 1, now_us); return;
        case SIDE_TAP:      cc.write_control(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_TAP, 1, now_us); return;
        case SIDE_PANIC:    release_all(now_us); mm.panic(); return;
        case SIDE_REVERB:
            set_page(current_page == PAGE_REVERB ? before_reverb : (uint8_t)PAGE_REVERB, now_us);
            return;
        default: return;
    }
}

void Launchpad::set_page(uint8_t page, uint32_t now_us){
    if (page == current_page) return;
    // Leaving the note page releases what it holds: a note-off has to come
    // from the pad that played it, and that pad means something else now.
    if (current_page == PAGE_NOTE) release_all(now_us);
    if (page == PAGE_REVERB) before_reverb = current_page;
    clear_down = 0;
    current_page = page;
}

// The note page --------------------------------------------------------------

// Chromatic, as a Launchpad's own note mode is: a semitone a column, so a
// row is a run up a string, and LP_ROW_OFFSET semitones a row, so the rows
// are the strings of a neck tuned in fourths, the bottom-left pad the tonic. The scale is painted on the pads rather than
// built into them, so a note outside it plays as it does on a Launchpad.
uint8_t Launchpad::pitch_of(uint8_t row, uint8_t col) const {
    const LaunchpadConfig& c = config();
    const int16_t pitch = (int16_t)global_key::tonic(c.octave) + 12 * shift + row * LP_ROW_OFFSET + col;
    if (pitch < 0 || pitch > 127) return NO_NOTE;
    return (uint8_t)pitch;
}

void Launchpad::note_pad(uint8_t row, uint8_t col, uint8_t velocity, bool down, uint32_t now_us){
    const uint8_t i = (uint8_t)(row * GRID + col);
    if (down){
        if (held_note[i] != NO_NOTE) return;      // a repeated note-on for a pad still down
        const uint8_t pitch = pitch_of(row, col);
        if (pitch == NO_NOTE) return;
        const LaunchpadConfig& c = config();
        const uint8_t channel = c.channel ? c.channel : 1;
        held_note[i] = pitch;
        held_channel[i] = channel;
        emit_note(MIDI_NOTE_ON, channel, pitch, velocity ? velocity : 1, now_us);
        return;
    }
    if (held_note[i] == NO_NOTE) return;
    const uint8_t pitch = held_note[i], channel = held_channel[i];
    held_note[i] = NO_NOTE;
    // Two pads can hold one pitch - the layout overlaps - and a note-off
    // while the other is still down would end a note somebody is holding.
    for (uint8_t k = 0; k < PADS; k++){
        if (held_note[k] == pitch && held_channel[k] == channel) return;
    }
    emit_note(MIDI_NOTE_OFF, channel, pitch, 0, now_us);
}

void Launchpad::release_all(uint32_t now_us){
    for (uint8_t i = 0; i < PADS; i++){
        if (held_note[i] == NO_NOTE) continue;
        const uint8_t pitch = held_note[i], channel = held_channel[i];
        held_note[i] = NO_NOTE;
        bool still = false;
        for (uint8_t k = 0; k < PADS; k++) if (held_note[k] == pitch && held_channel[k] == channel) still = true;
        if (!still) emit_note(MIDI_NOTE_OFF, channel, pitch, 0, now_us);
    }
    // A button held through a panic is not down any more as far as the
    // patch is concerned: it sent its 0 here.
    for (uint8_t s = 0; s < LP_WIDGETS; s++){
        if (!buttons_down[s]) continue;
        const LaunchpadWidget& g = config().widgets[s];
        for (uint8_t r = 0; r < GRID; r++){
            if (buttons_down[s] & (uint8_t)(1u << r)){
                emit_cc(g.channel ? g.channel : 1, (uint8_t)(g.cc + r), 0, now_us);
            }
        }
        buttons_down[s] = 0;
    }
}

void Launchpad::emit_note(uint8_t type, uint8_t channel, uint8_t note, uint8_t velocity, uint32_t now_us){
    const MidiEvent e = {type, channel, note, velocity};
    mm.deliver_midi(active_port, e, now_us);
}

// A CC takes the path a cable's CC takes (control/midi_dispatch.h): the
// binding table first, so a widget can be learned and bound, then the graph.
void Launchpad::emit_cc(uint8_t channel, uint8_t number, uint8_t value, uint32_t now_us){
    if (cc.observe(active_port, channel, number, value, now_us)) return;
    const MidiEvent e = {MIDI_CONTROL_CHANGE, channel, number, value};
    mm.deliver_midi(active_port, e, now_us);
}

// The key page -------------------------------------------------------------------
//
// Rows 7 and 6 are a keyboard - the black keys above the white - rows 4 and 3
// the scales in ScaleId order, rows 1 and 0 the register. Rows 5 and 2 are
// the gaps between them.

// The pitch class a white or black key is, or 0xFF for a gap.
static uint8_t keyboard_pitch_class(uint8_t row, uint8_t col){
    static const uint8_t white[8] = {0, 2, 4, 5, 7, 9, 11, 0xFF};
    static const uint8_t black[8] = {1, 3, 0xFF, 6, 8, 10, 0xFF, 0xFF};
    if (row == 6) return white[col];
    if (row == 7) return black[col];
    return 0xFF;
}

static uint8_t scale_at(uint8_t row, uint8_t col){
    const uint8_t index = row == 4 ? col : (row == 3 ? (uint8_t)(col + 8u) : 0xFF);
    if (index == 0xFF) return SCALE_NONE;
    const uint8_t id = (uint8_t)(SCALE_MAJOR + index);
    return id < SCALE_COUNT ? id : (uint8_t)SCALE_NONE;
}

static uint8_t register_at(uint8_t row, uint8_t col){
    const uint8_t octave = row == 1 ? (uint8_t)(col + 1u) : (row == 0 ? (uint8_t)(col + 9u) : 0);
    return octave <= KEY_MAX_OCTAVE ? octave : 0;
}

void Launchpad::key_pad(uint8_t row, uint8_t col, uint32_t now_us){
    const uint8_t pc = keyboard_pitch_class(row, col);
    if (pc != 0xFF){ cc.write_control(CC_TARGET_KEY, 0, CC_KEY_ROOT, pc, now_us); return; }
    const uint8_t scale = scale_at(row, col);
    if (scale != SCALE_NONE){ cc.write_control(CC_TARGET_KEY, 0, CC_KEY_SCALE, scale, now_us); return; }
    const uint8_t octave = register_at(row, col);
    if (octave != 0) cc.write_control(CC_TARGET_KEY, 0, CC_KEY_OCTAVE, octave, now_us);
}

// The widget pages --------------------------------------------------------------

void Launchpad::widget_pad(uint8_t row, uint8_t col, bool down, uint32_t now_us){
    const uint8_t slot = (uint8_t)(widgets_page * WIDGET_COLUMNS + col);
    if (slot >= LP_WIDGETS) return;
    const LaunchpadWidget& g = config().widgets[slot];
    const uint8_t channel = g.channel ? g.channel : 1;
    const uint8_t bit = (uint8_t)(1u << row);
    switch (g.kind){
        case LP_WIDGET_FADER:
            if (!down) return;
            fader_value[slot] = fader_level(row);
            emit_cc(channel, g.cc, fader_value[slot], now_us);
            return;
        case LP_WIDGET_BUTTONS:
            if (down) buttons_down[slot] |= bit; else buttons_down[slot] &= (uint8_t)~bit;
            emit_cc(channel, (uint8_t)(g.cc + row), down ? 127 : 0, now_us);
            return;
        case LP_WIDGET_TOGGLES:
            if (!down) return;
            toggles_on[slot] ^= bit;
            emit_cc(channel, (uint8_t)(g.cc + row), (toggles_on[slot] & bit) ? 127 : 0, now_us);
            return;
        default:
            return;
    }
}

// Where a fader is drawn: at the module's own value of the target its CC
// moves, when it moves one - so a fader bound to a parameter an LFO is also
// sweeping shows the sweep - and otherwise where it was last pressed.
uint8_t Launchpad::fader_shown(uint8_t slot, const LaunchpadWidget& g) const {
    const uint8_t channel = g.channel ? g.channel : 1;
    const Patch& p = patches.active();
    for (uint8_t i = 0; i < N_CC_MAP; i++){
        const CcMapping& m = p.cc_map[i];
        if (m.source_mask == 0 || m.cc != g.cc) continue;
        if ((m.source_mask & active_port) == 0) continue;
        if (m.channel != 0 && m.channel != channel) continue;
        uint16_t value = 0, lo = 0, hi = 0;
        if (!cc.read_control(m.target_kind, m.target_index, m.param, value)) break;
        if (m.min != 0 || m.max != 0){ lo = m.min; hi = m.max; }
        else if (!cc.target_range(m, lo, hi)) break;
        if (hi <= lo) break;
        if (value <= lo) return 0;
        if (value >= hi) return 7;
        return (uint8_t)(((uint32_t)(value - lo) * 7u + (uint32_t)(hi - lo) / 2u) / (uint32_t)(hi - lo));
    }
    return (uint8_t)(((uint32_t)fader_value[slot] * 7u + 63u) / 127u);
}

// The tempo page -------------------------------------------------------------------
//
// Row 7 is the transport, row 5 the nudges, rows 3 and 2 a sixteen-step bar
// across the clock's range, row 0 the beat in the bar.

void Launchpad::tempo_pad(uint8_t row, uint8_t col, uint32_t now_us){
    if (row == 7){
        switch (col){
            case 0: cc.write_control(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_START, 1, now_us); return;
            case 1: cc.write_control(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_STOP, 1, now_us); return;
            case 2: cc.write_control(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_CONTINUE, 1, now_us); return;
            case 3: cc.write_control(CC_TARGET_TRANSPORT, 0, CC_TRANSPORT_TAP, 1, now_us); return;
            case 7: release_all(now_us); mm.panic(); return;
            default: return;
        }
    }
    if (row == 5){
        static const int16_t NUDGE[8] = {-10, -5, -1, 0, 0, 1, 5, 10};
        if (NUDGE[col]) nudge_tempo(NUDGE[col], now_us);
        return;
    }
    if (row == 2 || row == 3){
        set_tempo(bar_tempo((uint8_t)((row - 2u) * 8u + col)), now_us);
    }
}

void Launchpad::nudge_tempo(int16_t by, uint32_t now_us){
    set_tempo((int32_t)mm.clock().bpm() + by, now_us);
}

void Launchpad::set_tempo(int32_t bpm, uint32_t now_us){
    if (bpm < CLOCK_MIN_BPM) bpm = CLOCK_MIN_BPM;
    if (bpm > CLOCK_MAX_BPM) bpm = CLOCK_MAX_BPM;
    cc.write_control(CC_TARGET_CLOCK, 0, CC_CLOCK_TEMPO, (uint16_t)bpm, now_us);
}

// The reverb page -----------------------------------------------------------------
//
// Columns 1 to 4 are faders, bottom row first as a widget fader is; the
// bottom-right block empties the tank on the press.

void Launchpad::reverb_pad(uint8_t row, uint8_t col, bool down){
    if (is_clear_pad(row, col)){
        const uint8_t bit = (uint8_t)(1u << (row * 2u + col - REVERB_CLEAR_COL));
        if (!down){ clear_down &= (uint8_t)~bit; return; }
        if (clear_down == 0) fx.clear();
        clear_down |= bit;
        return;
    }
    if (down && col < IReverb::CONTROLS) fx.set(col, fader_level(row));
}

// Output ---------------------------------------------------------------------------

void Launchpad::service(uint32_t now_us){
    follow_config(now_us);
    if (active_port == 0) return;
    // Asked every interval whether linked or not: unlinked, it is how a
    // Launchpad plugged in later is found; linked, it is the heartbeat whose
    // absence says it has gone.
    if (!ever_asked || (uint32_t)(now_us - last_inquiry_us) >= INQUIRY_INTERVAL_US) send_inquiry(now_us);
    if (is_linked && (uint32_t)(now_us - last_seen_us) >= LINK_TIMEOUT_US){
        release_all(now_us);
        is_linked = false;
        seen_model = LP_MODEL_AUTO;           // what comes back may be another model
        invalidate();
    }
    if (!is_linked) return;
    if ((uint32_t)(now_us - last_led_us) < led_interval()) return;
    last_led_us = now_us;
    draw(now_us);
    flush(now_us);
}

void Launchpad::draw(uint32_t now_us){
    for (uint8_t i = 0; i < LEDS; i++) want[i] = OFF;
    draw_frame(now_us);
    switch (current_page){
        case PAGE_NOTE:    draw_note(); break;
        case PAGE_KEY:     draw_key(); break;
        case PAGE_WIDGETS: draw_widgets(); break;
        case PAGE_TEMPO:   draw_tempo(now_us); break;
        case PAGE_REVERB:  draw_reverb(); break;
        default: break;
    }
}

// The top row and the right column: the same on every page.
void Launchpad::draw_frame(uint32_t now_us){
    (void)now_us;
    static const uint8_t PAGE_BUTTON[TOP_PAGES] = {TOP_NOTE, TOP_KEY, TOP_WIDGETS, TOP_TEMPO};
    for (uint8_t p = 0; p < TOP_PAGES; p++) set(button_id(PAGE_BUTTON[p]), p == current_page ? WHITE : DIM);
    // The arrows light where they do something.
    const bool vertical = current_page != PAGE_WIDGETS && current_page != PAGE_REVERB;
    set(button_id(TOP_UP), vertical ? DIM : OFF);
    set(button_id(TOP_DOWN), vertical ? DIM : OFF);
    const bool horizontal = current_page == PAGE_WIDGETS || current_page == PAGE_TEMPO;
    set(button_id(TOP_LEFT), horizontal ? DIM : OFF);
    set(button_id(TOP_RIGHT), horizontal ? DIM : OFF);
    if (current_page == PAGE_WIDGETS){
        set(button_id(TOP_LEFT), widgets_page > 0 ? WHITE : DIM);
        set(button_id(TOP_RIGHT), widgets_page + 1 < WIDGET_PAGES ? WHITE : DIM);
    }

    const bool running = mm.clock().running();
    const uint32_t count = mm.clock().count();
    const bool on_beat = running && (count % CLOCK_SUBTICKS_PER_QUARTER) < CLOCK_SUBTICKS_PER_QUARTER / 2u;
    set(side_id(SIDE_PLAY), running ? GREEN : dim(RED));
    set(side_id(SIDE_CONTINUE), (side_down & (1u << SIDE_CONTINUE)) ? YELLOW : dim(YELLOW));
    set(side_id(SIDE_TAP), on_beat ? WHITE : DIM);
    set(side_id(SIDE_PANIC), (side_down & (1u << SIDE_PANIC)) ? PINK : dim(PINK));
    set(side_id(SIDE_REVERB), current_page == PAGE_REVERB ? WHITE : (fx.running() ? VIOLET : dim(VIOLET)));
    set(LOGO_ID, WHITE);
}

// The key painted on the neck: the roots pink, the rest of the scale blue,
// a note outside the scale unlit, a held pad green. An empty mask is
// chromatic, as it is everywhere (midi/scale.h), so every pad is in it.
void Launchpad::draw_note(){
    uint16_t mask = global_key::mask() & 0x0FFF;
    if (mask == 0) mask = 0x0FFF;
    const uint8_t root = global_key::root();
    for (uint8_t row = 0; row < GRID; row++){
        for (uint8_t col = 0; col < GRID; col++){
            const uint8_t i = (uint8_t)(row * GRID + col);
            const uint8_t id = led_id(row, col);
            if (held_note[i] != NO_NOTE){ set(id, GREEN); continue; }
            const uint8_t pitch = pitch_of(row, col);
            if (pitch == NO_NOTE){ set(id, OFF); continue; }
            const uint8_t interval = (uint8_t)((pitch + 12u - root) % 12u);
            if (interval == 0) set(id, PINK);
            else if (mask & (uint16_t)(1u << interval)) set(id, BLUE);
            else set(id, OFF);
        }
    }
}

void Launchpad::draw_key(){
    const uint16_t mask = global_key::mask();
    const uint8_t root = global_key::root();
    const uint8_t scale = global_key::id();
    const uint8_t octave = global_key::octave();
    for (uint8_t row = 0; row < GRID; row++){
        for (uint8_t col = 0; col < GRID; col++){
            const uint8_t id = led_id(row, col);
            const uint8_t pc = keyboard_pitch_class(row, col);
            if (pc != 0xFF){
                const bool in_scale = (mask & (uint16_t)(1u << ((pc + 12u - root) % 12u))) != 0;
                if (pc == root) set(id, BLUE);
                else if (in_scale) set(id, row == 6 ? WHITE : GREY);
                else set(id, row == 6 ? DIM : OFF);
                continue;
            }
            const uint8_t s = scale_at(row, col);
            if (s != SCALE_NONE){ set(id, s == scale ? ORANGE : dim(ORANGE)); continue; }
            const uint8_t o = register_at(row, col);
            if (o != 0){ set(id, o == octave ? MAGENTA : dim(MAGENTA)); continue; }
            set(id, OFF);
        }
    }
}

void Launchpad::draw_widgets(){
    const LaunchpadConfig& c = config();
    for (uint8_t col = 0; col < WIDGET_COLUMNS; col++){
        const uint8_t slot = (uint8_t)(widgets_page * WIDGET_COLUMNS + col);
        const LaunchpadWidget& g = c.widgets[slot];
        const uint8_t colour = g.colour ? g.colour : WIDGET_DEFAULT_COLOUR;
        for (uint8_t row = 0; row < GRID; row++){
            const uint8_t id = led_id(row, col);
            const uint8_t bit = (uint8_t)(1u << row);
            switch (g.kind){
                case LP_WIDGET_FADER:
                    set(id, row <= fader_shown(slot, g) ? colour : dim(colour));
                    break;
                case LP_WIDGET_BUTTONS:
                    set(id, (buttons_down[slot] & bit) ? colour : dim(colour));
                    break;
                case LP_WIDGET_TOGGLES:
                    set(id, (toggles_on[slot] & bit) ? colour : dim(colour));
                    break;
                default:
                    set(id, OFF);
                    break;
            }
        }
    }
}

void Launchpad::draw_tempo(uint32_t now_us){
    (void)now_us;
    const bool running = mm.clock().running();
    const uint32_t count = mm.clock().count();
    const uint16_t bpm = mm.clock().bpm();
    // Row 7: the transport.
    set(led_id(7, 0), running ? GREEN : dim(GREEN));
    set(led_id(7, 1), running ? dim(RED) : RED);
    set(led_id(7, 2), dim(YELLOW));
    const bool on_beat = running && (count % CLOCK_SUBTICKS_PER_QUARTER) < CLOCK_SUBTICKS_PER_QUARTER / 2u;
    set(led_id(7, 3), on_beat ? WHITE : DIM);
    set(led_id(7, 7), dim(PINK));
    // Row 5: the nudges, down in blue and up in orange.
    for (uint8_t col = 0; col < 3; col++) set(led_id(5, col), dim(BLUE));
    for (uint8_t col = 5; col < 8; col++) set(led_id(5, col), dim(ORANGE));
    // Rows 3 and 2: the bar.
    const uint32_t level = ((uint32_t)(bpm - CLOCK_MIN_BPM) * 15u + (CLOCK_MAX_BPM - CLOCK_MIN_BPM) / 2u)
                         / (CLOCK_MAX_BPM - CLOCK_MIN_BPM);
    for (uint8_t step = 0; step < 16; step++){
        set(led_id((uint8_t)(2u + step / 8u), (uint8_t)(step % 8u)), step <= level ? YELLOW : dim(YELLOW));
    }
    // Row 0: the beat in the bar.
    const uint32_t beat = (count / CLOCK_SUBTICKS_PER_QUARTER) % BEATS_PER_BAR;
    for (uint8_t b = 0; b < BEATS_PER_BAR; b++){
        set(led_id(0, b), running && b == beat ? (on_beat ? GREEN : dim(GREEN)) : (running ? dim(GREEN) : DIM));
    }
}

// The LEDs that differ from what the device shows, at most LEDS_PER_MESSAGE
// of them, as one message. The rest wait for the next interval.
void Launchpad::flush(uint32_t now_us){
    (void)now_us;
    uint16_t at = 0;
    tx[at++] = 0xF0;
    tx[at++] = NOVATION_0; tx[at++] = NOVATION_1; tx[at++] = NOVATION_2;
    tx[at++] = 0x02;
    tx[at++] = device_byte();
    tx[at++] = 0x03;
    uint8_t n = 0;
    for (uint8_t id = 11; id < LEDS && n < LEDS_PER_MESSAGE; id++){
        const uint8_t col = (uint8_t)(id % 10u);
        if (col == 0) continue;                       // 20, 30 .. 90: not ours
        if (id > 89 && id < 91) continue;
        if (want[id] == shown[id]) continue;
        tx[at++] = 0x00;                               // static colour
        tx[at++] = id;
        tx[at++] = want[id];
        shown[id] = want[id];
        n++;
    }
    if (n == 0) return;
    tx[at++] = 0xF7;
    midi.send_sysex(active_port, tx, at);
}

void Launchpad::draw_reverb(){
    static const uint8_t COLOUR[IReverb::CONTROLS] = {BLUE, CYAN, YELLOW, GREEN};
    for (uint8_t col = 0; col < IReverb::CONTROLS; col++){
        const uint8_t level = (uint8_t)(((uint32_t)fx.get(col) * 7u + 63u) / 127u);
        for (uint8_t row = 0; row < GRID; row++) set(led_id(row, col), row <= level ? COLOUR[col] : dim(COLOUR[col]));
    }
    for (uint8_t row = 0; row < REVERB_CLEAR_ROWS; row++){
        for (uint8_t col = REVERB_CLEAR_COL; col < GRID; col++) set(led_id(row, col), clear_down ? RED : dim(RED));
    }
}
