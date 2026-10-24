#include <unity.h>
#include <stdlib.h>
#include <vector>
#include <new>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "master.h"
#include "control/macros.h"
#include "control/cc_mapper.h"
#include "control/control_sum.h"
#include "control/mod_matrix.h"
#include "control/nrpn.h"
#include "control/midi_dispatch.h"
#include "patch/patch_manager.h"
#include "patch/patch_codec.h"
#include "protocol/sysex.h"
#include "protocol/sysex_handler.h"
#include "surface/launchpad.h"
#include "midi/global_key.h"
#include "node/registry.h"
#include "hal/midi_types.h"
#include "algorithm/midi/transpose.h"

static size_t g_allocations = 0;
void* operator new(size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { g_allocations++; void* p = malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

void setUp() {}
void tearDown() {}

// A Launchpad as the module's panel (surface/launchpad.h): linked by its
// answer to a device inquiry, playing the key, setting it, moving the clock,
// and sending CCs the binding table can learn.

static constexpr uint8_t LP = mmMIDI_HOST_LAUNCHPAD; // the cable the Launchpad is on
static constexpr uint8_t SYNTH = mmMIDI_USB_0;       // where what it plays leaves
static constexpr uint8_t CONTROL = MIDI_CONTROL_PORT;

struct Rig {
    FakeGpio gpio;
    RecordingMidiOut midi;
    FakeEeprom eeprom;
    FakeLeds led_driver;
    HalTap panel;
    MixedModeMaster master;
    StatusLeds leds;
    PatchStore store;
    PatchManager patches;
    Macros macros;
    CcMapper cc;
    ControlSum sum;
    ModMatrix mod;
    NrpnDecoder nrpn;
    Monitor monitor;
    Launchpad surface;
    SysexHandler sysex;
    MidiInputQueue queue;
    uint32_t now;

    Rig() : gpio(), midi(), eeprom(), led_driver(), panel(gpio, midi),
            master(panel, panel), leds(led_driver), store(eeprom),
            patches(master, store, leds), macros(), cc(patches, master, macros), sum(cc), mod(patches, cc, sum),
            nrpn(patches, cc), monitor(master, panel, leds), surface(patches, master, cc, panel),
            sysex(patches, master, store, leds, panel, cc, mod, macros, sum, monitor), queue(), now(0) {
        sysex.attach(surface);
    }

    // One loop of main.cpp: the queue dispatched, the controllers applied, a
    // pass, the surface serviced after it.
    void loop(uint32_t step_us = 1000){
        dispatch_midi(queue, sysex, nrpn, cc, master, surface, now);
        cc.apply(now);
        master.pass(now);
        surface.service(now);
        now += step_us;
    }

    // Enough loops for every LED frame the surface owes to have gone out.
    void settle(){ for (int i = 0; i < 20; i++) loop(5000); }

    void in(uint8_t source, uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2){
        queue.push(source, MidiEvent{type, channel, d1, d2});
    }
    // The Launchpad's own messages, as programmer mode sends them: the grid
    // as notes on channel 1, the buttons as CCs.
    void pad(uint8_t row, uint8_t col, bool down, uint8_t velocity = 100){
        in(LP, down ? MIDI_NOTE_ON : MIDI_NOTE_OFF, 1, Launchpad::led_id(row, col), down ? velocity : 0);
    }
    void top(uint8_t index, bool down = true){ in(LP, MIDI_CONTROL_CHANGE, 1, Launchpad::top_id(index), down ? 127 : 0); }
    void side(uint8_t row, bool down = true){ in(LP, MIDI_CONTROL_CHANGE, 1, Launchpad::side_id(row), down ? 127 : 0); }

    // A Launchpad answering what the module asked, as the real ones do: the
    // inquiry with a family that does not tell a Mini from a Pro, and the
    // layout readback only where it carries the device's own id.
    void answer(uint8_t device = Launchpad::DEVICE_MINI_MK3, uint8_t source = LP){
        const uint8_t family = device == Launchpad::DEVICE_X ? 0x03 : 0x13;
        const uint8_t reply[] = {0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x20, 0x29, family, 0x01, 0x00, 0x00,
                                 0x00, 0x01, 0x00, 0x00, 0xF7};
        // Through the protocol handler, which is where a transport delivers
        // every SysEx and which hands on what is not its own.
        const size_t asked = midi.sysex.size();
        sysex.deliver_sysex(source, reply, (uint16_t)sizeof reply, now);
        for (size_t i = asked; i < midi.sysex.size(); i++){
            const std::vector<uint8_t>& b = midi.sysex[i].bytes;
            if (midi.sysex[i].target != source || b.size() != 8 || b[1] != 0x00 || b[5] != device
                || b[6] != Launchpad::COMMAND_LAYOUT) continue;
            const uint8_t layout[] = {0xF0, 0x00, 0x20, 0x29, 0x02, device, Launchpad::COMMAND_LAYOUT, 0x05, 0xF7};
            sysex.deliver_sysex(source, layout, (uint16_t)sizeof layout, now);
            return;
        }
    }

    // Loads a patch with a Launchpad on LP, a MIDI in reading it onto note
    // bus 0 and a MIDI out playing that bus to SYNTH, then links it.
    void link(const LaunchpadConfig& lp, uint8_t device = Launchpad::DEVICE_MINI_MK3){
        Patch p = empty_patch();
        p.midi_in[0] = MidiInConfig{LP, 0, one_bus(0)};
        p.midi_out[0] = MidiOutConfig{SYNTH, 0, one_bus(0)};
        p.launchpad = lp;
        TEST_ASSERT_EQUAL(APPLY_OK, patches.apply(p, default_globals(), now));
        loop();                       // the surface sees its cable and asks
        answer(device);
        loop();                       // linked: programmer mode, then the first frame
        midi.clear();
    }

    // Every message out to the Launchpad that lights LEDs, in order.
    std::vector<const RecordingMidiOut::Sysex*> led_messages() const {
        std::vector<const RecordingMidiOut::Sysex*> found;
        for (const RecordingMidiOut::Sysex& s : midi.sysex){
            if (s.target == LP && s.bytes.size() > 7 && s.bytes[1] == 0x00 && s.bytes[2] == 0x20
                && s.bytes[3] == 0x29 && s.bytes[6] == 0x03) found.push_back(&s);
        }
        return found;
    }
    // The colour the last LED message set `id` to, or -1.
    int sent_colour(uint8_t id) const {
        int colour = -1;
        for (const RecordingMidiOut::Sysex* s : led_messages()){
            for (size_t at = 7; at + 2 < s->bytes.size(); at += 3){
                if (s->bytes[at] == 0x00 && s->bytes[at + 1] == id) colour = s->bytes[at + 2];
            }
        }
        return colour;
    }
    std::vector<RecordingMidiOut::Message> notes_out() const {
        std::vector<RecordingMidiOut::Message> found;
        for (const RecordingMidiOut::Message& m : midi.messages){
            if (m.target == SYNTH && (m.type == MIDI_NOTE_ON || m.type == MIDI_NOTE_OFF)) found.push_back(m);
        }
        return found;
    }
};

static LaunchpadConfig on_host(){
    LaunchpadConfig lp = no_launchpad();
    lp.port = LP;
    return lp;
}

// ---------------------------------------------------------------------------
// Linking
// ---------------------------------------------------------------------------
static void test_the_surface_asks_who_is_on_its_cable_and_links_on_the_answer() {
    Rig rig;
    Patch p = empty_patch();
    p.launchpad = on_host();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    rig.loop();
    // A device inquiry, on the Launchpad's cable and nowhere else.
    TEST_ASSERT_EQUAL(1, rig.midi.sysex.size());
    TEST_ASSERT_EQUAL(LP, rig.midi.sysex[0].target);
    TEST_ASSERT_EQUAL(0x7E, rig.midi.sysex[0].bytes[1]);
    TEST_ASSERT_EQUAL(0x01, rig.midi.sysex[0].bytes[4]);
    TEST_ASSERT_FALSE(rig.surface.linked());
    // Nothing is lit before anything answers.
    for (int i = 0; i < 100; i++) rig.loop();
    TEST_ASSERT_EQUAL(0, rig.led_messages().size());

    rig.answer(Launchpad::DEVICE_MINI_MK3);
    TEST_ASSERT_TRUE(rig.surface.linked());
    TEST_ASSERT_EQUAL(LP_MODEL_MINI_MK3, rig.surface.model());
    // Programmer mode, with the Mini's device byte.
    const RecordingMidiOut::Sysex& mode = rig.midi.sysex.back();
    const uint8_t expected[] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7};
    TEST_ASSERT_EQUAL(sizeof expected, mode.bytes.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, mode.bytes.data(), sizeof expected);
    // And the first frame: the logo lit, the note page's button white.
    rig.settle();
    TEST_ASSERT_TRUE(rig.led_messages().size() >= 1);
    TEST_ASSERT_EQUAL(Launchpad::WHITE, rig.sent_colour(Launchpad::LOGO_ID));
    TEST_ASSERT_EQUAL(Launchpad::WHITE, rig.sent_colour(Launchpad::top_id(5)));
}

// A Pro answers the inquiry as a Mini does, so the inquiry alone links
// nothing: each model's layout readback goes out, and only the Pro's comes
// back. Its programmer-mode switch is the Pro's - the Mini's would leave it
// in its own modes.
static void test_a_pro_is_told_from_a_mini_by_its_own_answer() {
    Rig rig;
    Patch p = empty_patch();
    p.launchpad = on_host();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    rig.loop();
    const uint8_t inquiry_reply[] = {0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x20, 0x29, 0x13, 0x01, 0x00, 0x00,
                                     0x00, 0x01, 0x00, 0x00, 0xF7};
    rig.midi.clear();
    rig.sysex.deliver_sysex(LP, inquiry_reply, (uint16_t)sizeof inquiry_reply, rig.now);
    TEST_ASSERT_FALSE(rig.surface.linked());
    TEST_ASSERT_EQUAL(3, rig.midi.sysex.size());
    for (const RecordingMidiOut::Sysex& s : rig.midi.sysex){
        TEST_ASSERT_EQUAL(LP, s.target);
        TEST_ASSERT_EQUAL(8, s.bytes.size());
        TEST_ASSERT_EQUAL(Launchpad::COMMAND_LAYOUT, s.bytes[6]);
    }
    for (int i = 0; i < 100; i++) rig.loop();
    TEST_ASSERT_EQUAL(0, rig.led_messages().size());

    const uint8_t layout[] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x00, 0x03, 0x00, 0x00, 0xF7};
    rig.sysex.deliver_sysex(LP, layout, (uint16_t)sizeof layout, rig.now);
    TEST_ASSERT_TRUE(rig.surface.linked());
    TEST_ASSERT_EQUAL(LP_MODEL_PRO_MK3, rig.surface.model());
    const uint8_t expected[] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x0E, 0x01, 0xF7};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, rig.midi.sysex.back().bytes.data(), sizeof expected);
    rig.settle();
    TEST_ASSERT_TRUE(rig.led_messages().size() >= 1);
    for (const RecordingMidiOut::Sysex* s : rig.led_messages()) TEST_ASSERT_EQUAL(Launchpad::DEVICE_PRO_MK3, s->bytes[5]);
    // Its heartbeat is its own readback's answer, and that keeps it the Pro.
    for (int i = 0; i < 3000; i++) rig.loop();
    rig.answer(Launchpad::DEVICE_PRO_MK3);
    TEST_ASSERT_EQUAL(LP_MODEL_PRO_MK3, rig.surface.model());
}

static void test_a_named_model_is_used_when_the_answer_names_none() {
    Rig rig;
    LaunchpadConfig lp = on_host();
    lp.model = LP_MODEL_X;
    rig.link(lp, 0x77);                  // a device that answers nothing but the inquiry
    TEST_ASSERT_TRUE(rig.surface.linked());
    TEST_ASSERT_EQUAL(LP_MODEL_X, rig.surface.model());
}

static void test_silence_unlinks_and_an_answer_relinks() {
    Rig rig;
    rig.link(on_host());
    for (uint32_t i = 0; i < 7000; i++) rig.loop();      // seven seconds without an answer
    TEST_ASSERT_FALSE(rig.surface.linked());
    // Still asking, so a Launchpad plugged in later is found.
    size_t inquiries = 0;
    for (const RecordingMidiOut::Sysex& s : rig.midi.sysex) if (s.bytes[1] == 0x7E) inquiries++;
    TEST_ASSERT_TRUE(inquiries >= 3);
    rig.midi.clear();
    rig.answer();
    TEST_ASSERT_TRUE(rig.surface.linked());
    // Put back into programmer mode, and lit again from scratch.
    TEST_ASSERT_EQUAL(0x0E, rig.midi.sysex.back().bytes[6]);
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::WHITE, rig.sent_colour(Launchpad::LOGO_ID));
}

static void test_taking_the_launchpad_off_its_cable_returns_it_to_live_mode() {
    Rig rig;
    rig.link(on_host());
    Patch p = rig.patches.active();
    p.launchpad = no_launchpad();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, rig.patches.globals(), rig.now));
    rig.loop();
    TEST_ASSERT_FALSE(rig.surface.linked());
    const RecordingMidiOut::Sysex& mode = rig.midi.sysex.back();
    TEST_ASSERT_EQUAL(LP, mode.target);
    TEST_ASSERT_EQUAL(0x0E, mode.bytes[6]);
    TEST_ASSERT_EQUAL(0x00, mode.bytes[7]);
    // And a pad on that cable is a note on that cable again: the MIDI in
    // reading it hears the raw pad number.
    rig.midi.clear();
    rig.pad(0, 0, true);
    rig.loop();
    TEST_ASSERT_EQUAL(1, rig.notes_out().size());
    TEST_ASSERT_EQUAL(Launchpad::led_id(0, 0), rig.notes_out()[0].d1);
}

// ---------------------------------------------------------------------------
// The note page
// ---------------------------------------------------------------------------
static void test_the_grid_plays_the_key() {
    Rig rig;
    rig.link(on_host());
    GlobalSettings g = rig.patches.globals();
    g.scale = SCALE_MAJOR; g.root = 0;
    rig.patches.set_globals(g, rig.now);
    // Bottom left is the tonic in the key's register: middle C.
    rig.pad(0, 0, true, 90);
    rig.loop();
    std::vector<RecordingMidiOut::Message> out = rig.notes_out();
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_ON, out[0].type);
    TEST_ASSERT_EQUAL(60, out[0].d1);
    TEST_ASSERT_EQUAL(90, out[0].d2);
    TEST_ASSERT_EQUAL(1, out[0].channel);
    // A row up the neck is a fourth; two columns along, two semitones.
    rig.pad(1, 0, true);
    rig.pad(0, 2, true);
    rig.loop();
    out = rig.notes_out();
    TEST_ASSERT_EQUAL(3, out.size());
    TEST_ASSERT_EQUAL(65, out[1].d1);
    TEST_ASSERT_EQUAL(62, out[2].d1);
    // The raw pad notes never reach the graph.
    for (const RecordingMidiOut::Message& m : out) TEST_ASSERT_NOT_EQUAL(11, m.d1);
}

static void test_a_pad_releases_the_pitch_it_played_whatever_the_key_does() {
    Rig rig;
    LaunchpadConfig lp = on_host();
    lp.channel = 5;
    rig.link(lp);
    rig.pad(0, 0, true);
    rig.loop();
    // The key moves under the held pad.
    rig.cc.write_control(CC_TARGET_KEY, 0, CC_KEY_ROOT, 7, rig.now);
    rig.pad(0, 0, false);
    rig.loop();
    std::vector<RecordingMidiOut::Message> out = rig.notes_out();
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[1].type);
    TEST_ASSERT_EQUAL(60, out[1].d1);
    TEST_ASSERT_EQUAL(5, out[1].channel);
    // And the next press is in the new key.
    rig.pad(0, 0, true);
    rig.loop();
    TEST_ASSERT_EQUAL(67, rig.notes_out()[2].d1);
}

static void test_two_pads_on_one_pitch_release_it_once() {
    Rig rig;
    rig.link(on_host());                 // (1, 0) and (0, 5) are the same pitch
    rig.pad(1, 0, true);
    rig.pad(0, 5, true);
    rig.loop();
    TEST_ASSERT_EQUAL(2, rig.notes_out().size());
    rig.pad(1, 0, false);
    rig.loop();
    TEST_ASSERT_EQUAL(2, rig.notes_out().size());        // still held by the other pad
    rig.pad(0, 5, false);
    rig.loop();
    TEST_ASSERT_EQUAL(3, rig.notes_out().size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, rig.notes_out()[2].type);
}

static void test_the_arrows_move_the_register() {
    Rig rig;
    rig.link(on_host());
    rig.top(0);                          // up
    rig.loop();
    TEST_ASSERT_EQUAL(1, rig.surface.octave_shift());
    rig.pad(0, 0, true);
    rig.loop();
    TEST_ASSERT_EQUAL(72, rig.notes_out()[0].d1);
    for (int i = 0; i < 10; i++){ rig.top(1); rig.loop(); }   // down, past the floor
    TEST_ASSERT_EQUAL(-Launchpad::OCTAVE_SHIFT_MAX, rig.surface.octave_shift());
}

// A Pro's buttons are its printed ones: the note page on Note, the other
// pages on Session, Chord and Custom, up and down at the top of its left
// column and left and right on its own arrows.
static void test_a_pro_plays_its_printed_buttons() {
    Rig rig;
    rig.link(on_host(), Launchpad::DEVICE_PRO_MK3);
    TEST_ASSERT_EQUAL(LP_MODEL_PRO_MK3, rig.surface.model());
    auto cc = [&](uint8_t id){ rig.in(LP, MIDI_CONTROL_CHANGE, 1, id, 127); rig.loop(); };
    cc(93);                                  // Session
    TEST_ASSERT_EQUAL(Launchpad::PAGE_KEY, rig.surface.page());
    cc(95);                                  // Chord
    TEST_ASSERT_EQUAL(Launchpad::PAGE_WIDGETS, rig.surface.page());
    cc(92);                                  // right
    TEST_ASSERT_EQUAL(1, rig.surface.widget_page());
    cc(96);                                  // Custom
    TEST_ASSERT_EQUAL(Launchpad::PAGE_TEMPO, rig.surface.page());
    cc(94);                                  // Note
    TEST_ASSERT_EQUAL(Launchpad::PAGE_NOTE, rig.surface.page());
    cc(80);                                  // up
    TEST_ASSERT_EQUAL(1, rig.surface.octave_shift());
    cc(70); cc(70);                          // down
    TEST_ASSERT_EQUAL(-1, rig.surface.octave_shift());
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::WHITE, rig.surface.led(94));
    TEST_ASSERT_EQUAL(Launchpad::DIM, rig.surface.led(93));
    TEST_ASSERT_EQUAL(Launchpad::DIM, rig.surface.led(80));
    TEST_ASSERT_EQUAL(Launchpad::OFF, rig.surface.led(98));
}

static void test_the_note_page_is_a_neck_in_fourths() {
    Rig rig;
    rig.link(on_host());
    rig.pad(0, 0, true);                 // the tonic
    rig.pad(0, 1, true);                 // a semitone along
    rig.pad(1, 0, true);                 // a fourth up the neck: the bottom row's sixth pad
    rig.pad(7, 7, true);                 // the top corner
    rig.loop();
    std::vector<RecordingMidiOut::Message> out = rig.notes_out();
    TEST_ASSERT_EQUAL(4, out.size());
    TEST_ASSERT_EQUAL(60, out[0].d1);
    TEST_ASSERT_EQUAL(61, out[1].d1);
    TEST_ASSERT_EQUAL(60 + 5, out[2].d1);
    TEST_ASSERT_EQUAL(60 + 7 * 5 + 7, out[3].d1);
}

static void test_the_note_page_paints_the_key_on_the_neck() {
    Rig rig;
    rig.link(on_host());
    GlobalSettings g = rig.patches.globals();
    g.scale = SCALE_MAJOR;               // C major, the tonic C4 bottom left
    rig.patches.set_globals(g, rig.now);
    rig.pad(0, 2, true);
    rig.settle();
    // The root pink, a scale note blue, a note outside the scale off, the
    // held pad green; up the neck the colours follow the pitches.
    TEST_ASSERT_EQUAL(Launchpad::PINK, rig.surface.led(Launchpad::led_id(0, 0)));   // C
    TEST_ASSERT_EQUAL(Launchpad::OFF, rig.surface.led(Launchpad::led_id(0, 1)));    // C#
    TEST_ASSERT_EQUAL(Launchpad::GREEN, rig.surface.led(Launchpad::led_id(0, 2)));  // D, held
    TEST_ASSERT_EQUAL(Launchpad::OFF, rig.surface.led(Launchpad::led_id(0, 3)));    // D#
    TEST_ASSERT_EQUAL(Launchpad::BLUE, rig.surface.led(Launchpad::led_id(0, 4)));   // E
    TEST_ASSERT_EQUAL(Launchpad::BLUE, rig.surface.led(Launchpad::led_id(0, 5)));   // F
    TEST_ASSERT_EQUAL(Launchpad::OFF, rig.surface.led(Launchpad::led_id(0, 6)));    // F#
    TEST_ASSERT_EQUAL(Launchpad::BLUE, rig.surface.led(Launchpad::led_id(0, 7)));   // G
    TEST_ASSERT_EQUAL(Launchpad::BLUE, rig.surface.led(Launchpad::led_id(1, 0)));   // F
    TEST_ASSERT_EQUAL(Launchpad::PINK, rig.surface.led(Launchpad::led_id(1, 7)));   // C5
    TEST_ASSERT_EQUAL(Launchpad::GREEN, rig.sent_colour(Launchpad::led_id(0, 2)));
    // The note outside the scale plays all the same, as it does on a
    // Launchpad.
    rig.pad(0, 1, true);
    rig.loop();
    TEST_ASSERT_EQUAL(61, rig.notes_out()[1].d1);
    // A key change repaints it: in C minor the E goes dark and the D# lights.
    g.scale = SCALE_NATURAL_MINOR;
    rig.patches.set_globals(g, rig.now);
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::OFF, rig.surface.led(Launchpad::led_id(0, 4)));
    TEST_ASSERT_EQUAL(Launchpad::BLUE, rig.surface.led(Launchpad::led_id(0, 3)));
}

// ---------------------------------------------------------------------------
// The key page
// ---------------------------------------------------------------------------
static void test_the_key_page_sets_root_scale_and_register_through_the_globals() {
    Rig rig;
    rig.link(on_host());
    rig.top(4);                          // the key page
    rig.loop();
    TEST_ASSERT_EQUAL(Launchpad::PAGE_KEY, rig.surface.page());
    rig.pad(6, 1, true);                 // D, on the white row
    rig.loop();
    TEST_ASSERT_EQUAL(2, rig.patches.globals().root);
    TEST_ASSERT_EQUAL(2, global_key::root());
    rig.pad(7, 3, true);                 // F sharp
    rig.loop();
    TEST_ASSERT_EQUAL(6, rig.patches.globals().root);
    rig.pad(4, 1, true);                 // the second scale: natural minor
    rig.loop();
    TEST_ASSERT_EQUAL(SCALE_NATURAL_MINOR, rig.patches.globals().scale);
    rig.pad(3, 0, true);                 // the ninth: phrygian
    rig.loop();
    TEST_ASSERT_EQUAL(SCALE_PHRYGIAN, rig.patches.globals().scale);
    rig.pad(1, 2, true);                 // register 3
    rig.loop();
    TEST_ASSERT_EQUAL(3, rig.patches.globals().root_octave);
    rig.pad(0, 1, true);                 // register 10
    rig.loop();
    TEST_ASSERT_EQUAL(10, rig.patches.globals().root_octave);
    rig.top(1);                          // down: 9
    rig.loop();
    TEST_ASSERT_EQUAL(9, rig.patches.globals().root_octave);
    // A gap pad does nothing.
    rig.pad(7, 2, true);
    rig.loop();
    TEST_ASSERT_EQUAL(6, rig.patches.globals().root);
    // Drawn: the root blue, the chosen scale and register bright.
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::BLUE, rig.surface.led(Launchpad::led_id(7, 3)));
    TEST_ASSERT_EQUAL(Launchpad::ORANGE, rig.surface.led(Launchpad::led_id(3, 0)));
    TEST_ASSERT_EQUAL(Launchpad::dim(Launchpad::ORANGE), rig.surface.led(Launchpad::led_id(4, 0)));
    TEST_ASSERT_EQUAL(Launchpad::MAGENTA, rig.surface.led(Launchpad::led_id(0, 0)));
}

static void test_leaving_the_note_page_releases_what_it_holds() {
    Rig rig;
    rig.link(on_host());
    rig.pad(0, 0, true);
    rig.loop();
    rig.top(4);
    rig.loop();
    std::vector<RecordingMidiOut::Message> out = rig.notes_out();
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[1].type);
    // The same pad on the key page is a key, not a note.
    rig.pad(0, 0, true);
    rig.loop();
    TEST_ASSERT_EQUAL(2, rig.notes_out().size());
}

// ---------------------------------------------------------------------------
// The tempo page and the right column
// ---------------------------------------------------------------------------
static void test_the_tempo_page_nudges_and_sets_the_tempo() {
    Rig rig;
    rig.link(on_host());
    rig.top(7);                          // the tempo page
    rig.loop();
    rig.pad(5, 7, true);                 // +10
    rig.loop();
    TEST_ASSERT_EQUAL(130, rig.master.clock().bpm());
    TEST_ASSERT_EQUAL(130, rig.patches.globals().bpm);
    rig.pad(5, 2, true);                 // -1
    rig.loop();
    TEST_ASSERT_EQUAL(129, rig.master.clock().bpm());
    rig.top(0);                          // up: +1
    rig.top(2);                          // left: -10
    rig.loop();
    TEST_ASSERT_EQUAL(120, rig.master.clock().bpm());
    rig.pad(2, 0, true);                 // the bottom of the bar
    rig.loop();
    TEST_ASSERT_EQUAL(CLOCK_MIN_BPM, rig.master.clock().bpm());
    rig.pad(3, 7, true);                 // the top
    rig.loop();
    TEST_ASSERT_EQUAL(CLOCK_MAX_BPM, rig.master.clock().bpm());
    for (int i = 0; i < 5; i++){ rig.pad(5, 7, true); rig.loop(); }
    TEST_ASSERT_EQUAL(CLOCK_MAX_BPM, rig.master.clock().bpm());   // clamped, never refused
}

static void test_the_transport_is_on_the_tempo_page_and_the_right_column() {
    Rig rig;
    rig.link(on_host());
    // The module boots with its clock running; the one button stops it,
    // and starts it again.
    TEST_ASSERT_TRUE(rig.master.clock().running());
    rig.side(7);
    rig.loop();
    TEST_ASSERT_FALSE(rig.master.clock().running());
    rig.side(7);
    rig.loop();
    TEST_ASSERT_TRUE(rig.master.clock().running());
    rig.top(7);
    rig.loop();
    rig.pad(7, 0, true);                 // start
    rig.loop();
    TEST_ASSERT_TRUE(rig.master.clock().running());
    rig.pad(7, 1, true);                 // stop
    rig.loop();
    TEST_ASSERT_FALSE(rig.master.clock().running());
    rig.pad(7, 2, true);                 // continue
    rig.loop();
    TEST_ASSERT_TRUE(rig.master.clock().running());
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::GREEN, rig.surface.led(Launchpad::side_id(7)));
    TEST_ASSERT_EQUAL(Launchpad::GREEN, rig.surface.led(Launchpad::led_id(7, 0)));
}

static void test_the_tap_button_taps_the_tempo() {
    Rig rig;
    rig.link(on_host());
    for (int i = 0; i < 4; i++){
        rig.side(5);                     // tap
        rig.loop();
        rig.side(5, false);
        for (int t = 0; t < 599; t++) rig.loop();   // a beat at 100
    }
    TEST_ASSERT_EQUAL(100, rig.master.clock().bpm());
    // It is the patch's tempo, so the next write to the globals - a new
    // root from the key page - keeps it rather than undoing it.
    TEST_ASSERT_EQUAL(100, rig.patches.globals().bpm);
    rig.cc.write_control(CC_TARGET_KEY, 0, CC_KEY_ROOT, 7, rig.now);
    TEST_ASSERT_EQUAL(100, rig.master.clock().bpm());
}

static void test_panic_releases_the_grid_and_sweeps_the_ports() {
    Rig rig;
    rig.link(on_host());
    rig.pad(0, 0, true);
    rig.loop();
    rig.side(4);                         // panic
    rig.loop();
    std::vector<RecordingMidiOut::Message> out = rig.notes_out();
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL(MIDI_NOTE_OFF, out[1].type);
    bool swept = false;
    for (const RecordingMidiOut::Message& m : rig.midi.messages){
        if (m.type == MIDI_CONTROL_CHANGE && m.d1 == MIDI_CC_ALL_NOTES_OFF) swept = true;
    }
    TEST_ASSERT_TRUE(swept);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
static LaunchpadConfig with_widgets(){
    LaunchpadConfig lp = on_host();
    lp.widgets[0] = LaunchpadWidget{LP_WIDGET_FADER, 20, 1, Launchpad::ORANGE};
    lp.widgets[1] = LaunchpadWidget{LP_WIDGET_BUTTONS, 30, 2, 0};
    lp.widgets[2] = LaunchpadWidget{LP_WIDGET_TOGGLES, 40, 1, Launchpad::GREEN};
    lp.widgets[8] = LaunchpadWidget{LP_WIDGET_FADER, 50, 1, Launchpad::RED};
    return lp;
}

static std::vector<RecordingMidiOut::Message> ccs_out(const Rig& rig){
    std::vector<RecordingMidiOut::Message> found;
    for (const RecordingMidiOut::Message& m : rig.midi.messages){
        if (m.target == SYNTH && m.type == MIDI_CONTROL_CHANGE) found.push_back(m);
    }
    return found;
}

static void test_widgets_send_their_ccs_into_the_patch() {
    Rig rig;
    rig.link(with_widgets());
    rig.top(6);                          // the widgets page
    rig.loop();
    rig.pad(7, 0, true);                 // the fader's top
    rig.pad(3, 0, true);
    rig.pad(2, 1, true);                 // a button, down
    rig.pad(2, 1, false);                // and up
    rig.pad(0, 2, true);                 // a toggle, on
    rig.pad(0, 2, true);                 // and off
    rig.loop();
    std::vector<RecordingMidiOut::Message> out = ccs_out(rig);
    TEST_ASSERT_EQUAL(6, out.size());
    TEST_ASSERT_EQUAL(20, out[0].d1); TEST_ASSERT_EQUAL(127, out[0].d2); TEST_ASSERT_EQUAL(1, out[0].channel);
    TEST_ASSERT_EQUAL(20, out[1].d1); TEST_ASSERT_EQUAL(Launchpad::fader_level(3), out[1].d2);
    TEST_ASSERT_EQUAL(32, out[2].d1); TEST_ASSERT_EQUAL(127, out[2].d2); TEST_ASSERT_EQUAL(2, out[2].channel);
    TEST_ASSERT_EQUAL(32, out[3].d1); TEST_ASSERT_EQUAL(0, out[3].d2);
    TEST_ASSERT_EQUAL(40, out[4].d1); TEST_ASSERT_EQUAL(127, out[4].d2);
    TEST_ASSERT_EQUAL(40, out[5].d1); TEST_ASSERT_EQUAL(0, out[5].d2);
    // An empty column sends nothing.
    rig.pad(0, 7, true);
    rig.loop();
    TEST_ASSERT_EQUAL(6, ccs_out(rig).size());
    // The right arrow turns to the second page, whose first column is the
    // other fader.
    rig.top(3);
    rig.loop();
    TEST_ASSERT_EQUAL(1, rig.surface.widget_page());
    rig.pad(7, 0, true);
    rig.loop();
    TEST_ASSERT_EQUAL(50, ccs_out(rig)[6].d1);
}

static void test_a_widget_cc_is_the_binding_tables_before_it_is_the_graphs() {
    Rig rig;
    // A transposer whose semitone control CC 20 is bound to, on the
    // Launchpad's cable.
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{LP, 0, one_bus(0)};
    p.nodes[0] = node_config(ALGO_TRANSPOSE);
    p.nodes[0].in_buses[0] = one_bus(0);
    p.nodes[0].out_buses[0] = one_bus(1);
    p.n_nodes = 1;
    p.midi_out[0] = MidiOutConfig{SYNTH, 0, one_bus(1)};
    p.launchpad = with_widgets();
    CcMapping m = unused_mapping();
    m.source_mask = LP; m.channel = 1; m.cc = 20;
    m.target_kind = CC_TARGET_NODE; m.target_index = 0; m.param = Transpose::P_SEMITONES;
    p.cc_map[0] = m;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    rig.cc.reset();
    rig.loop(); rig.answer(); rig.loop();
    rig.midi.clear();
    rig.top(6);
    rig.loop();
    rig.pad(7, 0, true);                 // the fader's top: the parameter's maximum
    rig.loop();
    uint8_t value = 0;
    TEST_ASSERT_TRUE(rig.master.get_node_param(0, Transpose::P_SEMITONES, value));
    const ParamDescriptor* pd = registry::param(*rig.master.node_descriptor(0), Transpose::P_SEMITONES);
    TEST_ASSERT_EQUAL(pd->max, value);
    TEST_ASSERT_EQUAL(0, ccs_out(rig).size());          // consumed by the binding
    // The fader is drawn where the module has the parameter: the top.
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::ORANGE, rig.surface.led(Launchpad::led_id(7, 0)));
    // Something else moves the parameter, and the fader follows it down.
    rig.patches.set_param(0, Transpose::P_SEMITONES, pd->min, rig.now);
    rig.settle();
    TEST_ASSERT_EQUAL(Launchpad::ORANGE, rig.surface.led(Launchpad::led_id(0, 0)));
    TEST_ASSERT_EQUAL(Launchpad::dim(Launchpad::ORANGE), rig.surface.led(Launchpad::led_id(1, 0)));
}

static void test_a_widget_can_be_learned() {
    Rig rig;
    Patch p = empty_patch();
    p.midi_in[0] = MidiInConfig{LP, 0, one_bus(0)};
    p.nodes[0] = node_config(ALGO_TRANSPOSE);
    p.nodes[0].in_buses[0] = one_bus(0);
    p.nodes[0].out_buses[0] = one_bus(1);
    p.n_nodes = 1;
    p.launchpad = with_widgets();
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    rig.cc.reset();
    rig.loop(); rig.answer(); rig.loop();
    rig.top(6);
    rig.loop();
    rig.cc.learn_arm(0, CC_TARGET_NODE, 0, Transpose::P_SEMITONES, rig.now);
    rig.pad(4, 0, true);
    rig.loop();
    uint8_t slot = 0xFF;
    TEST_ASSERT_TRUE(rig.cc.take_learn_result(slot));
    TEST_ASSERT_EQUAL(0, slot);
    const CcMapping& learned = rig.patches.active().cc_map[0];
    TEST_ASSERT_EQUAL(20, learned.cc);
    TEST_ASSERT_EQUAL(LP, learned.source_mask);
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------
static void test_leds_go_out_in_bounded_messages_and_only_when_they_change() {
    Rig rig;
    rig.link(on_host());
    // The link left the first frame half sent: the rest follows at the LED
    // interval, no message longer than the budget, and then silence.
    for (int i = 0; i < 100; i++) rig.loop();
    std::vector<const RecordingMidiOut::Sysex*> sent = rig.led_messages();
    TEST_ASSERT_TRUE(sent.size() >= 2);
    for (const RecordingMidiOut::Sysex* s : sent){
        TEST_ASSERT_TRUE(s->bytes.size() <= Launchpad::LED_MESSAGE_MAX);
        TEST_ASSERT_EQUAL(0xF7, s->bytes.back());
    }
    rig.midi.clear();
    for (int i = 0; i < 100; i++) rig.loop();
    TEST_ASSERT_EQUAL(0, rig.led_messages().size());
    // A press lights one pad: one message, one LED.
    rig.pad(2, 2, true);
    for (int i = 0; i < 10; i++) rig.loop();
    sent = rig.led_messages();
    TEST_ASSERT_EQUAL(1, sent.size());
    TEST_ASSERT_EQUAL(7u + 3u + 1u, sent[0]->bytes.size());
    TEST_ASSERT_EQUAL(Launchpad::led_id(2, 2), sent[0]->bytes[8]);
    TEST_ASSERT_EQUAL(Launchpad::GREEN, sent[0]->bytes[9]);
}

static void test_a_din_cable_is_asked_for_less() {
    Rig rig;
    LaunchpadConfig lp = on_host();
    lp.port = mmMIDI_SERIAL_1;
    Patch p = empty_patch();
    p.launchpad = lp;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
    rig.loop();
    rig.answer(Launchpad::DEVICE_MINI_MK3, mmMIDI_SERIAL_1);
    rig.midi.clear();
    for (int i = 0; i < 100; i++) rig.loop();          // a tenth of a second
    size_t frames = 0;
    for (const RecordingMidiOut::Sysex& s : rig.midi.sysex) if (s.target == mmMIDI_SERIAL_1 && s.bytes[6] == 0x03) frames++;
    TEST_ASSERT_TRUE(frames <= 3);
    TEST_ASSERT_TRUE(frames >= 2);
}

// ---------------------------------------------------------------------------
// The patch, the image and the protocol
// ---------------------------------------------------------------------------
static void test_the_launchpad_round_trips_through_the_codec() {
    Patch p = empty_patch();
    p.launchpad = with_widgets();
    p.launchpad.model = LP_MODEL_PRO_MK3;
    p.launchpad.channel = 3;
    p.launchpad.octave = 4;
    uint8_t image[2048];
    size_t written = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(p, default_globals(), image, sizeof image, written));
    Patch back = empty_patch();
    GlobalSettings g;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::decode(image, written, back, g));
    TEST_ASSERT_EQUAL(LP, back.launchpad.port);
    TEST_ASSERT_EQUAL(LP_MODEL_PRO_MK3, back.launchpad.model);
    TEST_ASSERT_EQUAL(3, back.launchpad.channel);
    TEST_ASSERT_EQUAL(4, back.launchpad.octave);
    for (uint8_t i = 0; i < LP_WIDGETS; i++){
        TEST_ASSERT_EQUAL(p.launchpad.widgets[i].kind, back.launchpad.widgets[i].kind);
        TEST_ASSERT_EQUAL(p.launchpad.widgets[i].cc, back.launchpad.widgets[i].cc);
        TEST_ASSERT_EQUAL(p.launchpad.widgets[i].channel, back.launchpad.widgets[i].channel);
        TEST_ASSERT_EQUAL(p.launchpad.widgets[i].colour, back.launchpad.widgets[i].colour);
    }
    // No Launchpad costs six bytes, and a patch with none is a patch with none.
    Patch none = empty_patch();
    size_t without = 0;
    TEST_ASSERT_EQUAL(CODEC_OK, patch_codec::encode(none, default_globals(), image, sizeof image, without));
    TEST_ASSERT_EQUAL(written - 4u * 5u, without);
}

static void test_an_invalid_launchpad_refuses_the_whole_patch() {
    Rig rig;
    Patch p = empty_patch();
    p.launchpad.port = mmMIDI_HOST_1 | mmMIDI_SERIAL_1;        // two cables
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    TEST_ASSERT_EQUAL(LOAD_LAUNCHPAD_INVALID, rig.master.last_error());
    p.launchpad.port = MIDI_CONTROL_PORT;                       // the protocol's cable
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    p.launchpad.port = LP;
    p.launchpad.widgets[0] = LaunchpadWidget{LP_WIDGET_BUTTONS, 118, 1, 0};   // 118 + 7 is a mode message
    TEST_ASSERT_EQUAL(APPLY_INVALID, rig.patches.apply(p, default_globals(), 0));
    p.launchpad.widgets[0].cc = 112;
    TEST_ASSERT_EQUAL(APPLY_OK, rig.patches.apply(p, default_globals(), 0));
}

static void send(Rig& rig, uint8_t command, const std::vector<uint8_t>& args){
    std::vector<uint8_t> m = {0xF0, SYSEX_MANUFACTURER, rig.sysex.device_id(), command, SYSEX_PROTOCOL_VERSION};
    for (uint8_t b : args) m.push_back(b);
    m.push_back(0xF7);
    rig.sysex.deliver_sysex(CONTROL, m.data(), (uint16_t)m.size(), rig.now);
}

static void test_the_launchpad_is_set_and_read_over_the_wire() {
    Rig rig;
    rig.patches.apply(empty_patch(), default_globals(), 0);
    // The USB host Launchpad's bit fits a data byte, so the last one, which
    // carries the bit that does not, is clear.
    send(rig, SYSEX_SET_LAUNCHPAD, {0x40, LP_MODEL_X, 2, 6, 0x00});
    TEST_ASSERT_NOT_NULL(rig.midi.last_reply(SYSEX_ACK));
    TEST_ASSERT_EQUAL(LP, rig.patches.active().launchpad.port);
    TEST_ASSERT_EQUAL(LP_MODEL_X, rig.patches.active().launchpad.model);
    send(rig, SYSEX_GET_LAUNCHPAD, {});
    const RecordingMidiOut::Sysex* r = rig.midi.last_reply(SYSEX_LAUNCHPAD);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(0x40, r->bytes[5]);
    TEST_ASSERT_EQUAL(LP_MODEL_X, r->bytes[6]);
    TEST_ASSERT_EQUAL(2, r->bytes[7]);
    TEST_ASSERT_EQUAL(6, r->bytes[8]);
    TEST_ASSERT_EQUAL(0, r->bytes[9]);

    send(rig, SYSEX_SET_LP_WIDGET, {9, LP_WIDGET_TOGGLES, 70, 3, Launchpad::CYAN});
    TEST_ASSERT_NOT_NULL(rig.midi.last_reply(SYSEX_ACK));
    send(rig, SYSEX_GET_LP_WIDGET, {9});
    r = rig.midi.last_reply(SYSEX_LP_WIDGET);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(9, r->bytes[5]);
    TEST_ASSERT_EQUAL(LP_WIDGET_TOGGLES, r->bytes[6]);
    TEST_ASSERT_EQUAL(70, r->bytes[7]);
    TEST_ASSERT_EQUAL(3, r->bytes[8]);
    TEST_ASSERT_EQUAL(Launchpad::CYAN, r->bytes[9]);

    // A widget the module cannot hold is refused, and the patch is as it was.
    rig.midi.clear();
    send(rig, SYSEX_SET_LP_WIDGET, {9, LP_WIDGET_KINDS, 70, 3, 0});
    TEST_ASSERT_NOT_NULL(rig.midi.last_reply(SYSEX_NAK));
    TEST_ASSERT_EQUAL(LP_WIDGET_TOGGLES, rig.patches.active().launchpad.widgets[9].kind);
    send(rig, SYSEX_SET_LAUNCHPAD, {mmMIDI_SERIAL_1 | mmMIDI_SERIAL_2, 0, 0, 0, 0, 0});
    TEST_ASSERT_NOT_NULL(rig.midi.last_reply(SYSEX_NAK));
    TEST_ASSERT_EQUAL(LP, rig.patches.active().launchpad.port);

    // The state: nothing has answered yet.
    send(rig, SYSEX_GET_LP_STATE, {});
    r = rig.midi.last_reply(SYSEX_LP_STATE);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(0, r->bytes[5]);
    TEST_ASSERT_EQUAL(LP_MODEL_X, r->bytes[6]);
    rig.loop(); rig.answer(Launchpad::DEVICE_X); rig.loop();
    rig.top(7); rig.top(0); rig.loop();
    send(rig, SYSEX_GET_LP_STATE, {});
    r = rig.midi.last_reply(SYSEX_LP_STATE);
    TEST_ASSERT_EQUAL(1, r->bytes[5]);
    TEST_ASSERT_EQUAL(Launchpad::PAGE_TEMPO, r->bytes[7]);
    TEST_ASSERT_EQUAL(64, r->bytes[8]);
}

static void test_the_surface_never_allocates() {
    Rig rig;
    rig.link(with_widgets());
    // The recorder allocates a vector per SysEx message it keeps, which is
    // the test's cost and not the module's.
    const size_t before = g_allocations - rig.midi.sysex.size();
    rig.pad(0, 0, true); rig.loop();
    rig.top(4); rig.loop(); rig.pad(6, 1, true); rig.loop();
    rig.top(6); rig.loop(); rig.pad(3, 0, true); rig.loop();
    rig.top(7); rig.loop(); rig.pad(5, 7, true); rig.loop();
    for (int i = 0; i < 100; i++) rig.loop(10000);
    TEST_ASSERT_EQUAL(before, g_allocations - rig.midi.sysex.size());
}

int main(int, char**){
    UNITY_BEGIN();
    RUN_TEST(test_the_surface_asks_who_is_on_its_cable_and_links_on_the_answer);
    RUN_TEST(test_a_pro_is_told_from_a_mini_by_its_own_answer);
    RUN_TEST(test_a_named_model_is_used_when_the_answer_names_none);
    RUN_TEST(test_silence_unlinks_and_an_answer_relinks);
    RUN_TEST(test_taking_the_launchpad_off_its_cable_returns_it_to_live_mode);
    RUN_TEST(test_the_grid_plays_the_key);
    RUN_TEST(test_a_pad_releases_the_pitch_it_played_whatever_the_key_does);
    RUN_TEST(test_two_pads_on_one_pitch_release_it_once);
    RUN_TEST(test_the_arrows_move_the_register);
    RUN_TEST(test_a_pro_plays_its_printed_buttons);
    RUN_TEST(test_the_note_page_is_a_neck_in_fourths);
    RUN_TEST(test_the_note_page_paints_the_key_on_the_neck);
    RUN_TEST(test_the_key_page_sets_root_scale_and_register_through_the_globals);
    RUN_TEST(test_leaving_the_note_page_releases_what_it_holds);
    RUN_TEST(test_the_tempo_page_nudges_and_sets_the_tempo);
    RUN_TEST(test_the_transport_is_on_the_tempo_page_and_the_right_column);
    RUN_TEST(test_the_tap_button_taps_the_tempo);
    RUN_TEST(test_panic_releases_the_grid_and_sweeps_the_ports);
    RUN_TEST(test_widgets_send_their_ccs_into_the_patch);
    RUN_TEST(test_a_widget_cc_is_the_binding_tables_before_it_is_the_graphs);
    RUN_TEST(test_a_widget_can_be_learned);
    RUN_TEST(test_leds_go_out_in_bounded_messages_and_only_when_they_change);
    RUN_TEST(test_a_din_cable_is_asked_for_less);
    RUN_TEST(test_the_launchpad_round_trips_through_the_codec);
    RUN_TEST(test_an_invalid_launchpad_refuses_the_whole_patch);
    RUN_TEST(test_the_launchpad_is_set_and_read_over_the_wire);
    RUN_TEST(test_the_surface_never_allocates);
    return UNITY_END();
}
