#include "audio/rack.h"
#include <math.h>
#include "patch/patch_store.h"
#include "patch/patch_codec.h"

// The plugins' own parameters, in the order the plugin numbers them, with
// its defaults as 0..127. The names are the plugins' (lib/); the defaults
// are theirs too, but for Cumulus' amount: a reverb in a slot of its own is
// fully wet, and a dry path is a Thru slot beside it.
static const RackParamDesc CUMULUS[] = {
    {"Amount", 127}, {"Time", 64},  {"Diffuse", 95}, {"Tone", 95},  {"Input", 64},
    {"Clock", 127},  {"Warp", 0},   {"Speed", 38},   {"Glitch", 0},
};
static const RackParamDesc FERRIC[] = {
    {"Drive", 32}, {"Comp", 38},    {"Bits", 38},  {"Tone", 57},     {"Wow", 38},  {"Flutter", 32},
    {"Dropout", 19}, {"Chorus", 32}, {"Time", 44}, {"Feedback", 44}, {"Slap", 38}, {"Mix", 127},
};

// Cumulus carries 32 KB of delay memory and Ferric 72 KB, so every slot can
// be a reverb and two can be a tape: 272 KB of RAM2 between them.
static const RackPluginDesc PLUGINS[RACK_PLUGINS] = {
    {"none", "nothing: the slot is silent", 0, nullptr, N_RACK_SLOT},
    {"Thru", "the source as it is, through the low cut and at the level", 0, nullptr, N_RACK_SLOT},
    {"Cumulus", "the reverb of Mutable Instruments' Clouds, with a varispeed clock", 9, CUMULUS, N_RACK_SLOT},
    {"Ferric", "a worn tape machine: drive, wow and flutter, dropouts, chorus and a slap-back echo", 12, FERRIC, 2},
};

static_assert(sizeof CUMULUS / sizeof CUMULUS[0] + rack::SLOT_PARAMS <= RACK_PARAMS, "Cumulus fits a slot");
static_assert(sizeof FERRIC / sizeof FERRIC[0] + rack::SLOT_PARAMS <= RACK_PARAMS, "Ferric fits a slot");
static_assert(RACK_SOURCE_INPUT + RACK_CHANNELS <= RACK_SOURCE_SLOT, "the inputs stop short of the slots");
static_assert(RACK_SOURCE_SLOT + 2 * N_RACK_SLOT <= 0x80, "a source is a data byte");

// The slot's two, after a plugin's own.
static const RackParamDesc LOW_CUT = {"Low cut", 0};
static const RackParamDesc LEVEL = {"Level", rack::LEVEL_UNITY};

// Fixed: header, the device, and every slot at its widest.
static constexpr size_t HEADER_BYTES = 4 + 1 + 2;
static constexpr size_t DEVICE_BYTES = 2 + 2 + RACK_DEVICE_NAME_BYTES;
static constexpr size_t SLOT_BYTES = 6 + RACK_PARAMS;
static constexpr size_t IMAGE_MAX = HEADER_BYTES + DEVICE_BYTES + N_RACK_SLOT * SLOT_BYTES + 2;
static_assert(IMAGE_MAX <= RACK_STORE_BYTES, "the rack fits its region of the EEPROM");
static_assert(RACK_STORE_BASE + RACK_STORE_BYTES <= EEPROM_BYTES, "the rack's region is inside the EEPROM");

namespace rack {

const RackPluginDesc& plugin(uint8_t id){
    return PLUGINS[id < RACK_PLUGINS ? id : (uint8_t)RACK_PLUGIN_NONE];
}

uint8_t param_count(uint8_t id){
    if (id == RACK_PLUGIN_NONE || id >= RACK_PLUGINS) return 0;
    return (uint8_t)(plugin(id).n_own + SLOT_PARAMS);
}

uint8_t low_cut_param(uint8_t id){ return plugin(id).n_own; }
uint8_t level_param(uint8_t id){ return (uint8_t)(plugin(id).n_own + 1u); }

static const RackParamDesc* param_desc(uint8_t id, uint8_t p){
    if (p >= param_count(id)) return nullptr;
    const RackPluginDesc& d = plugin(id);
    if (p < d.n_own) return &d.own[p];
    return p == d.n_own ? &LOW_CUT : &LEVEL;
}

const char* param_name(uint8_t id, uint8_t p){
    const RackParamDesc* d = param_desc(id, p);
    return d ? d->name : nullptr;
}

uint8_t param_default(uint8_t id, uint8_t p){
    const RackParamDesc* d = param_desc(id, p);
    return d ? d->def : 0;
}

// 40 Hz to 2 kHz on an exponential travel, as the tape sketch's send has it,
// and off at the bottom.
float low_cut_hz(uint8_t value){
    if (value == 0) return 0.0f;
    return 40.0f * powf(50.0f, (float)value / 127.0f);
}

// A cube, unity at LEVEL_UNITY: a fader's travel spent where it is heard.
float level_gain(uint8_t value){
    const float v = (float)value / (float)LEVEL_UNITY;
    return v * v * v;
}

bool valid_source(uint8_t slot, uint8_t source){
    if (source == RACK_SOURCE_NONE) return true;
    if (source >= RACK_SOURCE_INPUT && source < RACK_SOURCE_INPUT + RACK_CHANNELS) return true;
    if (source < RACK_SOURCE_SLOT) return false;
    const uint8_t from = (uint8_t)((source - RACK_SOURCE_SLOT) / 2u);
    return from < slot;
}

bool valid_out(uint8_t out){ return out <= RACK_CHANNELS; }

void default_slot(RackSlot& s, uint8_t id){
    memset(&s, 0, sizeof s);
    s.plugin = id < RACK_PLUGINS ? id : (uint8_t)RACK_PLUGIN_NONE;
    for (uint8_t p = 0; p < param_count(s.plugin); p++) s.params[p] = param_default(s.plugin, p);
}

void default_rack(RackConfig& r){
    memset(&r, 0, sizeof r);
    for (uint8_t s = 0; s < N_RACK_SLOT; s++) default_slot(r.slots[s], RACK_PLUGIN_NONE);

    RackSlot& verb = r.slots[0];
    default_slot(verb, RACK_PLUGIN_CUMULUS);
    verb.source[0] = verb.source[1] = RACK_SOURCE_INPUT;           // input 1, into both sides
    verb.out[0] = 1; verb.out[1] = 2;

    RackSlot& tape = r.slots[1];
    default_slot(tape, RACK_PLUGIN_FERRIC);
    tape.source[0] = RACK_SOURCE_INPUT + 2; tape.source[1] = RACK_SOURCE_INPUT + 3;
    tape.out[0] = 3; tape.out[1] = 4;

    // The send: the tape through a low cut into a reverb, mixed in under it.
    RackSlot& send = r.slots[2];
    default_slot(send, RACK_PLUGIN_CUMULUS);
    send.source[0] = RACK_SOURCE_SLOT + 1 * 2; send.source[1] = RACK_SOURCE_SLOT + 1 * 2 + 1;
    send.out[0] = 3; send.out[1] = 4;
    send.params[low_cut_param(RACK_PLUGIN_CUMULUS)] = 44;
    send.params[level_param(RACK_PLUGIN_CUMULUS)] = 44;
}

static bool valid_slot(uint8_t index, const RackSlot& s){
    if (s.plugin >= RACK_PLUGINS) return false;
    for (uint8_t side = 0; side < 2; side++){
        if (!valid_source(index, s.source[side]) || !valid_out(s.out[side])) return false;
    }
    // Both sides into one output would want two inputs of its mixer.
    if (s.out[0] != 0 && s.out[0] == s.out[1]) return false;
    for (uint8_t p = 0; p < RACK_PARAMS; p++) if (s.params[p] > 127) return false;
    return true;
}

bool valid_rack(const RackConfig& r){
    uint8_t used[RACK_PLUGINS] = {};
    for (uint8_t s = 0; s < N_RACK_SLOT; s++){
        if (!valid_slot(s, r.slots[s])) return false;
        if (++used[r.slots[s].plugin] > plugin(r.slots[s].plugin).instances) return false;
    }
    return true;
}

size_t encode(const RackConfig& r, uint8_t* out, size_t capacity){
    if (capacity < IMAGE_MAX) return 0;
    size_t at = 0;
    const uint32_t magic = RACK_MAGIC;
    for (uint8_t i = 0; i < 4; i++) out[at++] = (uint8_t)(magic >> (8u * i));
    out[at++] = RACK_FORMAT_VERSION;
    at += 2;                                          // the length, below
    out[at++] = (uint8_t)r.device.vid; out[at++] = (uint8_t)(r.device.vid >> 8);
    out[at++] = (uint8_t)r.device.pid; out[at++] = (uint8_t)(r.device.pid >> 8);
    memcpy(&out[at], r.device.name, RACK_DEVICE_NAME_BYTES);
    at += RACK_DEVICE_NAME_BYTES;
    for (uint8_t s = 0; s < N_RACK_SLOT; s++){
        const RackSlot& slot = r.slots[s];
        const uint8_t n = param_count(slot.plugin);
        out[at++] = slot.plugin;
        out[at++] = slot.source[0]; out[at++] = slot.source[1];
        out[at++] = slot.out[0]; out[at++] = slot.out[1];
        out[at++] = n;
        for (uint8_t p = 0; p < n; p++) out[at++] = slot.params[p];
    }
    const size_t payload = at - HEADER_BYTES;
    out[5] = (uint8_t)payload; out[6] = (uint8_t)(payload >> 8);
    const uint16_t crc = patch_codec::crc16(out, at);
    out[at++] = (uint8_t)crc; out[at++] = (uint8_t)(crc >> 8);
    return at;
}

bool decode(const uint8_t* in, size_t length, RackConfig& r){
    if (length < HEADER_BYTES + DEVICE_BYTES + 2) return false;
    uint32_t magic = 0;
    for (uint8_t i = 0; i < 4; i++) magic |= (uint32_t)in[i] << (8u * i);
    if (magic != RACK_MAGIC || in[4] != RACK_FORMAT_VERSION) return false;
    const size_t payload = (size_t)in[5] | ((size_t)in[6] << 8);
    const size_t total = HEADER_BYTES + payload + 2;
    if (total != length) return false;
    const uint16_t crc = (uint16_t)(in[total - 2] | (in[total - 1] << 8));
    if (patch_codec::crc16(in, total - 2) != crc) return false;

    RackConfig decoded;
    memset(&decoded, 0, sizeof decoded);
    size_t at = HEADER_BYTES;
    const size_t end = total - 2;
    decoded.device.vid = (uint16_t)(in[at] | (in[at + 1] << 8)); at += 2;
    decoded.device.pid = (uint16_t)(in[at] | (in[at + 1] << 8)); at += 2;
    memcpy(decoded.device.name, &in[at], RACK_DEVICE_NAME_BYTES);
    at += RACK_DEVICE_NAME_BYTES;
    for (uint8_t s = 0; s < N_RACK_SLOT; s++){
        if (at + 6 > end) return false;
        RackSlot& slot = decoded.slots[s];
        slot.plugin = in[at++];
        slot.source[0] = in[at++]; slot.source[1] = in[at++];
        slot.out[0] = in[at++]; slot.out[1] = in[at++];
        const uint8_t n = in[at++];
        if (slot.plugin >= RACK_PLUGINS || n != param_count(slot.plugin) || at + n > end) return false;
        for (uint8_t p = 0; p < n; p++) slot.params[p] = in[at++];
    }
    if (at != end || !valid_rack(decoded)) return false;
    r = decoded;
    return true;
}

}  // namespace rack

// The rack ------------------------------------------------------------------

AudioRack::AudioRack(IEeprom& eeprom, IAudioHost& audio_host) :
    mem(eeprom), host(audio_host), config(), dirty_since_us(0), write_count(0), is_dirty(false)
{
    rack::default_rack(config);
}

void AudioRack::boot(){
    uint8_t image[RACK_STORE_BYTES];
    for (size_t i = 0; i < sizeof image; i++) image[i] = mem.read(RACK_STORE_BASE + i);
    const size_t length = HEADER_BYTES + ((size_t)image[5] | ((size_t)image[6] << 8)) + 2;
    if (length > sizeof image || !rack::decode(image, length, config)) rack::default_rack(config);
    push_all();
}

bool AudioRack::streaming() const {
    AudioStatus s;
    host.status(s);
    return s.state == AUDIO_STREAMING;
}

void AudioRack::push_all(){
    host.select(config.device);
    for (uint8_t s = 0; s < N_RACK_SLOT; s++) host.configure(s, config.slots[s]);
}

bool AudioRack::set_slot(uint8_t s, uint8_t plugin, const uint8_t source[2], const uint8_t out[2], uint32_t now_us){
    if (s >= N_RACK_SLOT) return false;
    RackSlot next = config.slots[s];
    if (plugin != next.plugin) rack::default_slot(next, plugin);
    next.source[0] = source[0]; next.source[1] = source[1];
    next.out[0] = out[0]; next.out[1] = out[1];
    RackConfig proposed = config;
    proposed.slots[s] = next;
    if (!rack::valid_rack(proposed)) return false;
    config.slots[s] = next;
    // A later slot reading a side this one no longer has is not wrong - it
    // reads silence - so nothing downstream is touched.
    host.configure(s, next);
    mark_dirty(now_us);
    return true;
}

bool AudioRack::set_param(uint8_t s, uint8_t param, uint8_t value, uint32_t now_us){
    if (s >= N_RACK_SLOT || value > 127) return false;
    if (param >= rack::param_count(config.slots[s].plugin)) return false;
    if (config.slots[s].params[param] == value) return true;
    config.slots[s].params[param] = value;
    host.set_param(s, param, value);
    mark_dirty(now_us);
    return true;
}

bool AudioRack::clear(uint8_t s){
    if (s >= N_RACK_SLOT) return false;
    host.clear(s);
    return true;
}

void AudioRack::select(const AudioDeviceId& device, uint32_t now_us){
    config.device = device;
    if (config.device.any()){ config.device.pid = 0; memset(config.device.name, 0, sizeof config.device.name); }
    host.select(config.device);
    mark_dirty(now_us);
}

void AudioRack::restore_defaults(uint32_t now_us){
    rack::default_rack(config);
    push_all();
    mark_dirty(now_us);
}

void AudioRack::mark_dirty(uint32_t now_us){
    if (is_dirty) return;
    is_dirty = true;
    dirty_since_us = now_us;
}

void AudioRack::service(uint32_t now_us){
    if (!is_dirty || (uint32_t)(now_us - dirty_since_us) < AUTOSAVE_SETTLE_US) return;
    save();
}

void AudioRack::save(){
    uint8_t image[RACK_STORE_BYTES];
    const size_t n = rack::encode(config, image, sizeof image);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) mem.write(RACK_STORE_BASE + i, image[i]);
    write_count++;
    is_dirty = false;
}
