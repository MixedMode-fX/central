#ifndef MMMC_PATCH_PATCH_STORE_H
#define MMMC_PATCH_PATCH_STORE_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "hal/ieeprom.h"
#include "patch/patch_codec.h"

// Where a patch lives between power cycles (#7).
//
// **EEPROM only, for now.** The Teensy 4.1's EEPROM is flash-emulated,
// EEPROM_BYTES of it, with limited write endurance. That budgets the current
// patch plus PATCH_SLOTS preset slots for Program Change recall (#11) - enough
// for recall to be useful, not a library. The microSD socket would hold
// hundreds of presets on dedicated SDIO pins and needs nothing from
// hardware.h, so it can be added later without touching the hardware surface;
// it is out of scope here because it is not declared hardware.
//
// Layout: PATCH_SLOTS fixed-size regions, each an independent
// patch_codec image with its own magic, version and CRC. A slot is therefore
// self-checking: a corrupted slot 2 cannot make slot 0 unreadable, and the
// module always has somewhere to boot from.
//
//   slot 0   the current patch, written on save
//   slot 1.. presets, recalled by Program Change
//
// **Writes are deliberate.** Nothing here is called on a parameter change.
// `mark_dirty()` notes that the live patch differs from slot 0 and starts a
// timer; `service()` writes it AUTOSAVE_SETTLE_US after the *first* edit of a
// burst, collapsing everything that happened in between into one write. A
// sweep of a knob is one write rather than a hundred, and - unlike a timer
// that restarts on every edit - a controller that never stops moving still
// gets its patch saved.

// The Teensy 4.1's emulated EEPROM. Stated here rather than taken from the
// library so the native tests size their fake the same way.
#define EEPROM_BYTES 4284

// Presets, including slot 0 (the current patch). Four slots of ~1 KB each is
// what EEPROM_BYTES affords; see the static_assert in the .cpp.
#define PATCH_SLOTS 4
#define PATCH_SLOT_BYTES (EEPROM_BYTES / PATCH_SLOTS)

enum StoreError : uint8_t {
    STORE_OK = 0,
    STORE_NO_SUCH_SLOT,
    STORE_EMPTY,             // the slot has never been written
    STORE_CORRUPT,           // magic, version or CRC did not check out
    STORE_TOO_LARGE,         // the patch does not fit a slot
};

class PatchStore {
    public:
        // How long after the first edit of a burst slot 0 is rewritten.
        // Two seconds collapses any plausible burst of edits into one write
        // and is far shorter than the gap before someone reaches for the
        // power switch.
        static constexpr uint32_t AUTOSAVE_SETTLE_US = 2000000;

        explicit PatchStore(IEeprom& eeprom);
        PatchStore(const PatchStore&) = delete;
        PatchStore& operator=(const PatchStore&) = delete;

        // Encodes and writes. Returns STORE_TOO_LARGE without touching the
        // slot if the image does not fit, so a patch is never half-written.
        StoreError save(uint8_t slot, const Patch& patch, const GlobalSettings& globals);
        // Reads and decodes. On any error `patch` and `globals` are left
        // alone: the caller falls back to the default patch (#7) rather than
        // running something partially valid.
        StoreError load(uint8_t slot, Patch& patch, GlobalSettings& globals) const;
        // True when the slot holds a decodable image.
        bool occupied(uint8_t slot) const;
        // Bytes the slot's image occupies, 0 when it is empty or corrupt.
        uint16_t used(uint8_t slot) const;

        // Wipes a slot so it reads back as STORE_EMPTY. Restoring defaults is
        // a host-side command - there is no button to hold at power-on.
        void erase(uint8_t slot);

        // The live patch differs from slot 0; service() will write it
        // AUTOSAVE_SETTLE_US after this, the first edit of the burst.
        void mark_dirty(uint32_t now_us);
        // Call once per main loop. Writes slot 0 once the settle time has
        // passed, and returns true on the pass where it did.
        bool service(uint32_t now_us, const Patch& patch, const GlobalSettings& globals);
        bool dirty() const { return is_dirty; }

        // Diagnostics, for the console and the red LED.
        uint32_t writes() const { return write_count; }
        StoreError last_error() const { return error; }

    private:
        size_t slot_base(uint8_t slot) const { return (size_t)slot * PATCH_SLOT_BYTES; }

        IEeprom& mem;
        uint32_t dirty_since_us;
        uint32_t write_count;
        StoreError error;
        bool is_dirty;
};

#endif
