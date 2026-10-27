#include <unity.h>
#include <string.h>
#include <vector>

#include "../fakes/fake_gpio.h"
#include "../fakes/fake_eeprom.h"
#include "../fakes/fake_leds.h"
#include "../fakes/recording_midi_out.h"
#include "audio/rack.h"
#include "master.h"
#include "patch/patch_store.h"
#include "patch/default_patch.h"
#include "protocol/sysex.h"
#include "protocol/sysex_handler.h"

void setUp() {}
void tearDown() {}

// The audio rack (audio/rack.h): what a slot may hold, how the rack is kept
// apart from the patches in the EEPROM, and the messages that read and write
// it.

static constexpr uint8_t CONTROL = MIDI_CONTROL_PORT;

static const uint8_t IN_1 = RACK_SOURCE_INPUT;
static uint8_t from_slot(uint8_t slot, uint8_t side){ return (uint8_t)(RACK_SOURCE_SLOT + slot * 2u + side); }

// ---------------------------------------------------------------------------
// The rack alone
// ---------------------------------------------------------------------------
struct RackRig {
    FakeEeprom eeprom;
    HeldAudio host;
    AudioRack rack;
    RackRig() : eeprom(), host(), rack(eeprom, host) {}
};

static void test_an_empty_eeprom_boots_the_default_rack_and_tells_the_host() {
    RackRig r;
    r.rack.boot();
    TEST_ASSERT_EQUAL(RACK_PLUGIN_CUMULUS, r.rack.slot(0).plugin);
    TEST_ASSERT_EQUAL(IN_1, r.rack.slot(0).source[0]);
    TEST_ASSERT_EQUAL(IN_1, r.rack.slot(0).source[1]);
    TEST_ASSERT_EQUAL(1, r.rack.slot(0).out[0]);
    TEST_ASSERT_EQUAL(2, r.rack.slot(0).out[1]);
    TEST_ASSERT_EQUAL(RACK_PLUGIN_FERRIC, r.rack.slot(1).plugin);
    // The send: a reverb reading the tape, mixed in under it on outputs 3/4.
    TEST_ASSERT_EQUAL(RACK_PLUGIN_CUMULUS, r.rack.slot(2).plugin);
    TEST_ASSERT_EQUAL(from_slot(1, 0), r.rack.slot(2).source[0]);
    TEST_ASSERT_EQUAL(from_slot(1, 1), r.rack.slot(2).source[1]);
    TEST_ASSERT_EQUAL(r.rack.slot(1).out[0], r.rack.slot(2).out[0]);
    TEST_ASSERT_TRUE(r.rack.slot(2).params[rack::low_cut_param(RACK_PLUGIN_CUMULUS)] > 0);
    TEST_ASSERT_EQUAL(RACK_PLUGIN_NONE, r.rack.slot(3).plugin);
    TEST_ASSERT_TRUE(r.rack.device().any());
    TEST_ASSERT_EQUAL(N_RACK_SLOT, r.host.configured());
    TEST_ASSERT_EQUAL(RACK_PLUGIN_FERRIC, r.host.slot(1).plugin);
}

static void test_a_slots_parameters_are_its_plugins_then_low_cut_and_level() {
    TEST_ASSERT_EQUAL(0, rack::param_count(RACK_PLUGIN_NONE));
    TEST_ASSERT_EQUAL(2, rack::param_count(RACK_PLUGIN_THRU));
    TEST_ASSERT_EQUAL(11, rack::param_count(RACK_PLUGIN_CUMULUS));
    TEST_ASSERT_EQUAL(14, rack::param_count(RACK_PLUGIN_FERRIC));
    TEST_ASSERT_EQUAL_STRING("Drive", rack::param_name(RACK_PLUGIN_FERRIC, 0));
    TEST_ASSERT_EQUAL_STRING("Mix", rack::param_name(RACK_PLUGIN_FERRIC, 11));
    TEST_ASSERT_EQUAL_STRING("Low cut", rack::param_name(RACK_PLUGIN_FERRIC, 12));
    TEST_ASSERT_EQUAL_STRING("Level", rack::param_name(RACK_PLUGIN_FERRIC, 13));
    TEST_ASSERT_NULL(rack::param_name(RACK_PLUGIN_FERRIC, 14));
    TEST_ASSERT_EQUAL(rack::LEVEL_UNITY, rack::param_default(RACK_PLUGIN_THRU, rack::level_param(RACK_PLUGIN_THRU)));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, rack::level_gain(rack::LEVEL_UNITY));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rack::low_cut_hz(0));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 2000.0f, rack::low_cut_hz(127));
    for (uint8_t id = 0; id < RACK_PLUGINS; id++) TEST_ASSERT_TRUE(rack::param_count(id) <= RACK_PARAMS);
}

static void test_a_slot_reads_only_inputs_and_earlier_slots() {
    RackRig r;
    r.rack.boot();
    const uint8_t outs[2] = {5, 6};
    const uint8_t later[2] = {from_slot(3, 0), from_slot(3, 1)};
    TEST_ASSERT_FALSE(r.rack.set_slot(2, RACK_PLUGIN_THRU, later, outs, 0));       // a later slot
    const uint8_t itself[2] = {from_slot(2, 0), from_slot(2, 1)};
    TEST_ASSERT_FALSE(r.rack.set_slot(2, RACK_PLUGIN_THRU, itself, outs, 0));      // itself: a loop
    const uint8_t beyond[2] = {(uint8_t)(RACK_SOURCE_INPUT + RACK_CHANNELS), IN_1};
    TEST_ASSERT_FALSE(r.rack.set_slot(2, RACK_PLUGIN_THRU, beyond, outs, 0));      // no such input
    const uint8_t earlier[2] = {from_slot(0, 0), from_slot(1, 1)};
    TEST_ASSERT_TRUE(r.rack.set_slot(2, RACK_PLUGIN_THRU, earlier, outs, 0));
    TEST_ASSERT_EQUAL(RACK_PLUGIN_THRU, r.host.slot(2).plugin);
    TEST_ASSERT_EQUAL(5, r.host.slot(2).out[0]);
}

static void test_a_slot_writes_two_different_outputs_or_none() {
    RackRig r;
    r.rack.boot();
    const uint8_t in[2] = {IN_1, IN_1};
    const uint8_t same[2] = {3, 3};
    TEST_ASSERT_FALSE(r.rack.set_slot(3, RACK_PLUGIN_THRU, in, same, 0));
    const uint8_t beyond[2] = {RACK_CHANNELS + 1, 0};
    TEST_ASSERT_FALSE(r.rack.set_slot(3, RACK_PLUGIN_THRU, in, beyond, 0));
    const uint8_t left_only[2] = {RACK_CHANNELS, 0};
    TEST_ASSERT_TRUE(r.rack.set_slot(3, RACK_PLUGIN_THRU, in, left_only, 0));
    const uint8_t plugin_none[2] = {0, 0};
    TEST_ASSERT_FALSE(r.rack.set_slot(N_RACK_SLOT, RACK_PLUGIN_THRU, in, plugin_none, 0));
    TEST_ASSERT_FALSE(r.rack.set_slot(3, RACK_PLUGINS, in, plugin_none, 0));
}

static void test_a_plugin_runs_in_no_more_slots_than_it_has_instances() {
    RackRig r;
    r.rack.boot();                                   // one tape already, in slot 1
    const uint8_t in[2] = {IN_1, IN_1};
    const uint8_t none[2] = {0, 0};
    const uint8_t limit = rack::plugin(RACK_PLUGIN_FERRIC).instances;
    uint8_t placed = 1;
    for (uint8_t s = 0; s < N_RACK_SLOT; s++){
        if (s == 1) continue;
        const bool ok = r.rack.set_slot(s, RACK_PLUGIN_FERRIC, in, none, 0);
        TEST_ASSERT_EQUAL(placed < limit, ok);
        if (ok) placed++;
    }
    TEST_ASSERT_EQUAL(limit, placed);
}

static void test_a_new_plugin_starts_at_its_defaults_and_the_same_one_keeps_its_settings() {
    RackRig r;
    r.rack.boot();
    TEST_ASSERT_TRUE(r.rack.set_param(1, 0, 120, 0));                  // the tape's drive
    const RackSlot was = r.rack.slot(1);
    const uint8_t outs[2] = {7, 8};
    TEST_ASSERT_TRUE(r.rack.set_slot(1, RACK_PLUGIN_FERRIC, was.source, outs, 0));
    TEST_ASSERT_EQUAL(120, r.rack.slot(1).params[0]);
    TEST_ASSERT_TRUE(r.rack.set_slot(1, RACK_PLUGIN_CUMULUS, was.source, outs, 0));
    TEST_ASSERT_EQUAL(rack::param_default(RACK_PLUGIN_CUMULUS, 0), r.rack.slot(1).params[0]);
    TEST_ASSERT_EQUAL(rack::param_default(RACK_PLUGIN_CUMULUS, 0), r.host.slot(1).params[0]);
}

static void test_a_parameter_must_exist_and_fit_a_fader() {
    RackRig r;
    r.rack.boot();
    TEST_ASSERT_FALSE(r.rack.set_param(0, rack::param_count(RACK_PLUGIN_CUMULUS), 10, 0));
    TEST_ASSERT_FALSE(r.rack.set_param(0, 0, 128, 0));
    TEST_ASSERT_FALSE(r.rack.set_param(3, 0, 10, 0));                  // the empty slot has none
    TEST_ASSERT_TRUE(r.rack.set_param(0, 2, 10, 0));
    TEST_ASSERT_EQUAL(10, r.host.slot(0).params[2]);
}

static void test_the_rack_is_saved_once_it_settles_and_comes_back() {
    RackRig r;
    r.rack.boot();
    TEST_ASSERT_TRUE(r.rack.set_param(1, 3, 99, 1000));
    TEST_ASSERT_TRUE(r.rack.set_param(1, 4, 98, 500000));
    AudioDeviceId scarlett;
    memset(&scarlett, 0, sizeof scarlett);
    scarlett.vid = 0x1235; scarlett.pid = 0x8219;
    strcpy(scarlett.name, "Scarlett 4i4 4th Gen");
    r.rack.select(scarlett, 600000);
    r.rack.service(1000000);
    TEST_ASSERT_EQUAL(0, r.rack.writes());                             // not settled
    r.rack.service(1000 + AudioRack::AUTOSAVE_SETTLE_US);
    TEST_ASSERT_EQUAL(1, r.rack.writes());
    TEST_ASSERT_FALSE(r.rack.dirty());
    TEST_ASSERT_EQUAL_STRING("Scarlett 4i4 4th Gen", r.host.selected().name);

    HeldAudio host;
    AudioRack again(r.eeprom, host);
    again.boot();
    TEST_ASSERT_EQUAL(99, again.slot(1).params[3]);
    TEST_ASSERT_EQUAL(98, again.slot(1).params[4]);
    TEST_ASSERT_EQUAL_HEX16(0x8219, again.device().pid);
    TEST_ASSERT_EQUAL_STRING("Scarlett 4i4 4th Gen", host.selected().name);
    TEST_ASSERT_EQUAL(99, host.slot(1).params[3]);
}

static void test_the_rack_and_the_patches_keep_out_of_each_others_bytes() {
    RackRig r;
    r.rack.boot();
    r.rack.set_param(0, 1, 7, 0);
    r.rack.service(AudioRack::AUTOSAVE_SETTLE_US);
    for (size_t i = 0; i < RACK_STORE_BASE; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, r.eeprom.read(i));

    // A patch written to the last slot leaves the rack readable.
    PatchStore store(r.eeprom);
    Patch p;
    default_patch(p);
    TEST_ASSERT_EQUAL(STORE_OK, store.save(PATCH_SLOTS - 1, p, default_globals()));
    HeldAudio host;
    AudioRack again(r.eeprom, host);
    again.boot();
    TEST_ASSERT_EQUAL(7, again.slot(0).params[1]);
}

static void test_a_corrupt_rack_boots_the_default_one() {
    RackRig r;
    r.rack.boot();
    r.rack.set_param(0, 1, 7, 0);
    r.rack.service(AudioRack::AUTOSAVE_SETTLE_US);
    r.eeprom.poke(RACK_STORE_BASE + 20, (uint8_t)(r.eeprom.read(RACK_STORE_BASE + 20) ^ 0x01));
    HeldAudio host;
    AudioRack again(r.eeprom, host);
    again.boot();
    TEST_ASSERT_EQUAL(rack::param_default(RACK_PLUGIN_CUMULUS, 1), again.slot(0).params[1]);
}

static void test_the_image_round_trips_and_refuses_anything_else() {
    RackConfig a;
    rack::default_rack(a);
    a.slots[3].plugin = RACK_PLUGIN_THRU;
    a.slots[3].params[0] = 12;
    uint8_t image[RACK_STORE_BYTES];
    const size_t n = rack::encode(a, image, sizeof image);
    TEST_ASSERT_TRUE(n > 0 && n <= RACK_STORE_BYTES);
    RackConfig b;
    rack::default_rack(b);
    TEST_ASSERT_TRUE(rack::decode(image, n, b));
    TEST_ASSERT_EQUAL(RACK_PLUGIN_THRU, b.slots[3].plugin);
    TEST_ASSERT_EQUAL(12, b.slots[3].params[0]);
    TEST_ASSERT_FALSE(rack::decode(image, n - 1, b));                  // truncated
    image[4] = RACK_FORMAT_VERSION + 1;                                // another version
    TEST_ASSERT_FALSE(rack::decode(image, n, b));
    TEST_ASSERT_EQUAL(0, rack::encode(a, image, 10));                  // no room
}

// ---------------------------------------------------------------------------
// Over the wire
// ---------------------------------------------------------------------------

// A socket with two devices on it, one only seen since boot.
class FakeUsbHost : public IUsbHost {
    public:
        void describe(IConsoleIo&) override {}
        uint8_t list(UsbDeviceEntry* out, uint8_t max) override {
            const UsbDeviceEntry entries[] = {
                {0x1235, 0x8219, USB_ROLE_AUDIO | USB_ROLE_HAS_AUDIO | USB_ROLE_MIDI | USB_ROLE_CONNECTED,
                 "Scarlett 4i4 4th Gen"},
                {0x1235, 0x0113, USB_ROLE_LAUNCHPAD, "Launchpad Mini MK3"},
            };
            uint8_t n = 0;
            for (const UsbDeviceEntry& e : entries) if (n < max) out[n++] = e;
            return n;
        }
};

struct WireRig {
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
    Monitor monitor;
    HeldAudio host;
    AudioRack rack;
    FakeUsbHost usb;
    SysexHandler sysex;

    WireRig() : gpio(), midi(), eeprom(), led_driver(), panel(gpio, midi),
                master(panel, panel), leds(led_driver), store(eeprom),
                patches(master, store, leds), macros(), cc(patches, master, macros), sum(cc), mod(patches, cc, sum),
                monitor(master, panel, leds), host(), rack(eeprom, host), usb(),
                sysex(patches, master, store, leds, panel, cc, mod, macros, sum, monitor) {
        rack.boot();
        sysex.attach(rack);
        sysex.attach(&usb);
    }

    void send(uint8_t command, const std::vector<uint8_t>& args = {}) {
        std::vector<uint8_t> m = {0xF0, SYSEX_MANUFACTURER, sysex.device_id(), command, SYSEX_PROTOCOL_VERSION};
        for (uint8_t b : args) m.push_back(b);
        m.push_back(0xF7);
        sysex.deliver_sysex(CONTROL, m.data(), (uint16_t)m.size(), 0);
    }
    bool acked() const { return midi.last_reply(SYSEX_ACK) != nullptr; }
    bool naked() const { return midi.last_reply(SYSEX_NAK) != nullptr; }
};

static uint16_t u16_at(const std::vector<uint8_t>& b, size_t at){
    return (uint16_t)(b[at] | (b[at + 1] << 7) | ((b[at + 2] & 0x03) << 14));
}
static std::string string_at(const std::vector<uint8_t>& b, size_t& at){
    const uint8_t n = b[at++];
    std::string s(b.begin() + (long)at, b.begin() + (long)(at + n));
    at += n;
    return s;
}

static void test_a_slot_is_read_and_written_over_the_wire() {
    WireRig w;
    w.send(SYSEX_GET_AUDIO_SLOT, {1});
    const RecordingMidiOut::Sysex* r = w.midi.last_reply(SYSEX_AUDIO_SLOT);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(1, r->bytes[5]);
    TEST_ASSERT_EQUAL(RACK_PLUGIN_FERRIC, r->bytes[6]);
    TEST_ASSERT_EQUAL(RACK_SOURCE_INPUT + 2, r->bytes[7]);
    TEST_ASSERT_EQUAL(3, r->bytes[9]);
    TEST_ASSERT_EQUAL(14, r->bytes[11]);
    TEST_ASSERT_EQUAL(rack::param_default(RACK_PLUGIN_FERRIC, 0), r->bytes[12]);

    w.send(SYSEX_SET_AUDIO_SLOT, {3, RACK_PLUGIN_THRU, IN_1, IN_1, 5, 6});
    TEST_ASSERT_TRUE(w.acked());
    TEST_ASSERT_EQUAL(RACK_PLUGIN_THRU, w.host.slot(3).plugin);
    w.midi.clear();
    w.send(SYSEX_SET_AUDIO_SLOT, {1, RACK_PLUGIN_THRU, from_slot(2, 0), IN_1, 5, 6});
    TEST_ASSERT_TRUE(w.naked());
    w.midi.clear();
    w.send(SYSEX_SET_AUDIO_PARAM, {3, 1, 64});
    TEST_ASSERT_TRUE(w.acked());
    TEST_ASSERT_EQUAL(64, w.rack.slot(3).params[1]);
    w.midi.clear();
    w.send(SYSEX_SET_AUDIO_PARAM, {3, 2, 64});                        // Thru has two
    TEST_ASSERT_TRUE(w.naked());
    w.midi.clear();
    w.send(SYSEX_AUDIO_CLEAR, {2});
    TEST_ASSERT_TRUE(w.acked());
    TEST_ASSERT_EQUAL(2, w.host.last_cleared());
}

static void test_the_plugins_describe_themselves() {
    WireRig w;
    w.send(SYSEX_PLUGIN_REQUEST);
    TEST_ASSERT_EQUAL(RACK_PLUGINS, w.midi.count_replies(SYSEX_PLUGIN));
    const RecordingMidiOut::Sysex* r = w.midi.last_reply(SYSEX_PLUGIN);
    const std::vector<uint8_t>& b = r->bytes;
    TEST_ASSERT_EQUAL(RACK_PLUGIN_FERRIC, b[7]);
    TEST_ASSERT_EQUAL(14, b[8]);
    TEST_ASSERT_EQUAL(rack::plugin(RACK_PLUGIN_FERRIC).instances, b[9]);
    size_t at = 10;
    TEST_ASSERT_EQUAL_STRING("Ferric", string_at(b, at).c_str());
    string_at(b, at);                                                 // the summary
    TEST_ASSERT_EQUAL(rack::param_default(RACK_PLUGIN_FERRIC, 0), b[at++]);
    TEST_ASSERT_EQUAL_STRING("Drive", string_at(b, at).c_str());
}

static void test_the_device_is_chosen_and_reported() {
    WireRig w;
    const char* name = "Scarlett 4i4 4th Gen";
    std::vector<uint8_t> args = {0x35, 0x24, 0x00, 0x19, 0x04, 0x02, (uint8_t)strlen(name)};
    for (const char* c = name; *c; c++) args.push_back((uint8_t)*c);
    w.send(SYSEX_SET_AUDIO_DEVICE, args);
    TEST_ASSERT_TRUE(w.acked());
    TEST_ASSERT_EQUAL_HEX16(0x1235, w.host.selected().vid);
    TEST_ASSERT_EQUAL_HEX16(0x8219, w.host.selected().pid);

    w.send(SYSEX_GET_AUDIO_STATE);
    const RecordingMidiOut::Sysex* r = w.midi.last_reply(SYSEX_AUDIO_STATE);
    TEST_ASSERT_NOT_NULL(r);
    const std::vector<uint8_t>& b = r->bytes;
    TEST_ASSERT_EQUAL(AUDIO_IDLE, b[5]);
    size_t at = 5 + 1 + 5 + 2;
    TEST_ASSERT_EQUAL_HEX16(0x1235, u16_at(b, at));
    TEST_ASSERT_EQUAL_HEX16(0x8219, u16_at(b, at + 3));
    at += 6;
    TEST_ASSERT_EQUAL_STRING(name, string_at(b, at).c_str());
    TEST_ASSERT_EQUAL(0, u16_at(b, at));                              // nothing held
}

static void test_the_usb_list_goes_out_whole() {
    WireRig w;
    w.send(SYSEX_USB_REQUEST);
    const RecordingMidiOut::Sysex* r = w.midi.last_reply(SYSEX_USB_DEVICES);
    TEST_ASSERT_NOT_NULL(r);
    const std::vector<uint8_t>& b = r->bytes;
    TEST_ASSERT_EQUAL(2, b[5]);
    TEST_ASSERT_EQUAL_HEX16(0x1235, u16_at(b, 6));
    TEST_ASSERT_EQUAL_HEX16(0x8219, u16_at(b, 9));
    TEST_ASSERT_TRUE(b[12] & USB_ROLE_CONNECTED);
    size_t at = 13;
    TEST_ASSERT_EQUAL_STRING("Scarlett 4i4 4th Gen", string_at(b, at).c_str());
    TEST_ASSERT_EQUAL_HEX16(0x0113, u16_at(b, at + 3));
}

static void test_a_full_usb_list_fits_one_reply() {
    // Every entry at its widest: the reply is budgeted against SYSEX_TX_MAX.
    TEST_ASSERT_TRUE(1u + 4u + 1u + USB_LIST_ENTRIES * (3u + 3u + 1u + 1u + USB_NAME_BYTES) + 1u <= SYSEX_TX_MAX);
}

static void test_a_module_without_a_rack_refuses_its_messages() {
    WireRig w;
    SysexHandler bare(w.patches, w.master, w.store, w.leds, w.panel, w.cc, w.mod, w.macros, w.sum, w.monitor);
    std::vector<uint8_t> m = {0xF0, SYSEX_MANUFACTURER, 0, SYSEX_GET_AUDIO_SLOT, SYSEX_PROTOCOL_VERSION, 0, 0xF7};
    bare.deliver_sysex(CONTROL, m.data(), (uint16_t)m.size(), 0);
    TEST_ASSERT_NOT_NULL(w.midi.last_reply(SYSEX_NAK));
    w.midi.clear();
    m = {0xF0, SYSEX_MANUFACTURER, 0, SYSEX_USB_REQUEST, SYSEX_PROTOCOL_VERSION, 0xF7};
    bare.deliver_sysex(CONTROL, m.data(), (uint16_t)m.size(), 0);
    const RecordingMidiOut::Sysex* r = w.midi.last_reply(SYSEX_USB_DEVICES);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(0, r->bytes[5]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_eeprom_boots_the_default_rack_and_tells_the_host);
    RUN_TEST(test_a_slots_parameters_are_its_plugins_then_low_cut_and_level);
    RUN_TEST(test_a_slot_reads_only_inputs_and_earlier_slots);
    RUN_TEST(test_a_slot_writes_two_different_outputs_or_none);
    RUN_TEST(test_a_plugin_runs_in_no_more_slots_than_it_has_instances);
    RUN_TEST(test_a_new_plugin_starts_at_its_defaults_and_the_same_one_keeps_its_settings);
    RUN_TEST(test_a_parameter_must_exist_and_fit_a_fader);
    RUN_TEST(test_the_rack_is_saved_once_it_settles_and_comes_back);
    RUN_TEST(test_the_rack_and_the_patches_keep_out_of_each_others_bytes);
    RUN_TEST(test_a_corrupt_rack_boots_the_default_one);
    RUN_TEST(test_the_image_round_trips_and_refuses_anything_else);
    RUN_TEST(test_a_slot_is_read_and_written_over_the_wire);
    RUN_TEST(test_the_plugins_describe_themselves);
    RUN_TEST(test_the_device_is_chosen_and_reported);
    RUN_TEST(test_the_usb_list_goes_out_whole);
    RUN_TEST(test_a_full_usb_list_fits_one_reply);
    RUN_TEST(test_a_module_without_a_rack_refuses_its_messages);
    return UNITY_END();
}
