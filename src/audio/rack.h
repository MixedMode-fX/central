#ifndef MMMC_AUDIO_RACK_H
#define MMMC_AUDIO_RACK_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "hal/ieeprom.h"
#include "hal/iaudio_host.h"

// The audio rack: the effects the module runs on the audio interface on its
// USB host socket, which interface that is, and where each effect reads and
// writes.
//
// **It is the module's, not a patch's.** A patch change swaps the graph; it
// never cuts off a reverb's tail or forgets which interface is the one. So
// the rack lives in its own region at the tail of the EEPROM
// (RACK_STORE_BASE), with its own magic, version and CRC, saved
// AUTOSAVE_SETTLE_US after the first change of a burst as the patch is.
//
// **Slots.** N_RACK_SLOT of them, each a plugin with a low cut in front of it
// and a level behind it. A side of a slot reads one interface input or one
// side of an *earlier* slot's output - earlier only, so the rack is a chain
// and never a loop - and each side writes one interface output, where every
// slot writing it is summed. A mono source into both sides of the reverb is
// how Clouds normals its inputs; a tape whose output is also read by a
// reverb slot through a low cut is a send:
//
//   in 3/4 -> Ferric ----------------------------> out 3/4
//                    \-> low cut -> Cumulus -----/
//
// **Parameters.** A slot's parameters are its plugin's, in the plugin's own
// order, and then the two every slot has: the low cut and the level. They are
// 0..127, as a fader stands for them, and travel as such: what a value means
// is the plugin's business (lib/), and the table here only names them.
//
// Nothing here knows what a Teensy is. The host (hal/iaudio_host.h) is told
// what a slot is and builds it.

// The plugins a slot can run. The ids are the stored rack's and the
// protocol's: appending is free, renumbering invalidates the stored rack -
// bump RACK_FORMAT_VERSION.
enum RackPlugin : uint8_t {
    RACK_PLUGIN_NONE    = 0,
    RACK_PLUGIN_THRU    = 1,   // the source, through the low cut, at the level
    RACK_PLUGIN_CUMULUS = 2,   // Clouds' reverb (lib/CumulusReverb)
    RACK_PLUGIN_FERRIC  = 3,   // a worn tape machine (lib/FerricTape)
    RACK_PLUGINS,
};

// Where one side of a slot reads from, as one byte: nothing, an interface
// input (RACK_SOURCE_INPUT + n for input n + 1) or a side of an earlier slot
// (RACK_SOURCE_SLOT + slot * 2 + side).
enum RackSource : uint8_t {
    RACK_SOURCE_NONE  = 0,
    RACK_SOURCE_INPUT = 1,
    RACK_SOURCE_SLOT  = 32,
};

// The stored rack's version, separate from the patch's: one moving does not
// invalidate the other. Bump it when RackSlot, the plugin ids or a plugin's
// parameters move.
#define RACK_FORMAT_VERSION 1
// "RACK", at the head of the stored image.
#define RACK_MAGIC 0x5241434Bu

struct RackParamDesc {
    const char* name;
    uint8_t def;             // 0..127
};

struct RackPluginDesc {
    const char* name;
    const char* summary;
    uint8_t n_own;           // the plugin's own parameters, before the two every slot has
    const RackParamDesc* own;
    // How many slots may run it at once: what its memory leaves room for
    // beside the others in the Teensy's RAM2 (hal/teensy/teensy_audio.cpp).
    uint8_t instances;
};

struct RackConfig {
    AudioDeviceId device;
    RackSlot slots[N_RACK_SLOT];
};

namespace rack {
    // The two parameters after a plugin's own.
    static constexpr uint8_t SLOT_PARAMS = 2;
    // A level of this is unity gain; 127 is +6 dB, on a cube.
    static constexpr uint8_t LEVEL_UNITY = 101;

    // The descriptor of a plugin id, or NONE's for one that is not a plugin.
    const RackPluginDesc& plugin(uint8_t id);
    // A slot's parameters with this plugin: its own and the slot's two, or
    // none for an empty slot.
    uint8_t param_count(uint8_t plugin);
    const char* param_name(uint8_t plugin, uint8_t param);
    uint8_t param_default(uint8_t plugin, uint8_t param);
    uint8_t low_cut_param(uint8_t plugin);
    uint8_t level_param(uint8_t plugin);

    // What the slot's two mean, for the host: the low cut's corner in Hz,
    // 0 when it is off, and the level's gain.
    float low_cut_hz(uint8_t value);
    float level_gain(uint8_t value);

    // Whether the rack holds what it may: every slot valid, and no plugin in
    // more slots than its instances.
    bool valid_rack(const RackConfig& r);
    // Whether `slot` may read `source`: an input the interface could have,
    // or an earlier slot.
    bool valid_source(uint8_t slot, uint8_t source);
    bool valid_out(uint8_t out);
    // A slot running `plugin` at its defaults, reading and writing nothing.
    void default_slot(RackSlot& s, uint8_t plugin);
    // What a module with nothing stored runs: Clouds' reverb from input 1 to
    // outputs 1 and 2, and the tape from inputs 3 and 4 to outputs 3 and 4
    // with a send to a second reverb.
    void default_rack(RackConfig& r);

    // The stored image: magic, version, length, the rack, CRC-16. Returns
    // the bytes written, or 0 when `capacity` is too small. decode() takes
    // exactly what encode() writes and refuses the rest, leaving `r` alone.
    size_t encode(const RackConfig& r, uint8_t* out, size_t capacity);
    bool decode(const uint8_t* in, size_t length, RackConfig& r);
}

class AudioRack {
    public:
        // How long after the first change of a burst the rack is written.
        static constexpr uint32_t AUTOSAVE_SETTLE_US = 2000000;

        AudioRack(IEeprom& eeprom, IAudioHost& host);
        AudioRack(const AudioRack&) = delete;
        AudioRack& operator=(const AudioRack&) = delete;

        // The stored rack, or the default one when the region does not check
        // out; then every slot and the chosen device go to the host.
        void boot();

        const RackSlot& slot(uint8_t s) const { return config.slots[s < N_RACK_SLOT ? s : 0]; }
        const AudioDeviceId& device() const { return config.device; }
        const RackConfig& rack() const { return config; }
        void status(AudioStatus& out) const { host.status(out); }
        bool streaming() const;

        // A slot's plugin and routing. A new plugin starts at its defaults;
        // the same plugin keeps its parameters. False, changing nothing, for
        // anything the slot may not hold - a plugin already in as many slots
        // as it has instances among them.
        bool set_slot(uint8_t s, uint8_t plugin, const uint8_t source[2], const uint8_t out[2], uint32_t now_us);
        bool set_param(uint8_t s, uint8_t param, uint8_t value, uint32_t now_us);
        bool clear(uint8_t s);
        // The interface to use, from the next time one is plugged in.
        void select(const AudioDeviceId& device, uint32_t now_us);
        void restore_defaults(uint32_t now_us);

        // Once per pass: writes the rack once it has settled.
        void service(uint32_t now_us);
        bool dirty() const { return is_dirty; }
        uint32_t writes() const { return write_count; }

    private:
        void mark_dirty(uint32_t now_us);
        void save();
        void push_all();

        IEeprom& mem;
        IAudioHost& host;
        RackConfig config;
        uint32_t dirty_since_us;
        uint32_t write_count;
        bool is_dirty;
};

#endif
