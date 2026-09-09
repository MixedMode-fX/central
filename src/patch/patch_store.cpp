#include "patch/patch_store.h"

// A slot has to hold the fixed part of an image - the header, the globals,
// every port and the node count - plus room for nodes worth having. Anything
// less and Program Change recall would be a promise rather than a feature.
static_assert(PATCH_SLOT_BYTES >= 256, "a preset slot must hold more than the header");
static_assert(PATCH_SLOTS * PATCH_SLOT_BYTES <= EEPROM_BYTES, "slots overrun the EEPROM");

// The encode buffer. One static block rather than a stack array, because a
// slot is a kilobyte and the Teensy's main stack is not the place for it.
// PatchStore is a single instance owned by the firmware, so there is no
// re-entrancy to worry about; the tests construct several and never overlap.
static uint8_t g_image[PATCH_SLOT_BYTES];

PatchStore::PatchStore(IEeprom& eeprom) :
    mem(eeprom), dirty_since_us(0), write_count(0), error(STORE_OK), is_dirty(false)
{}

StoreError PatchStore::save(uint8_t slot, const Patch& patch, const GlobalSettings& globals){
    if (slot >= PATCH_SLOTS) return error = STORE_NO_SUCH_SLOT;

    size_t written = 0;
    const CodecError e = patch_codec::encode(patch, globals, g_image, sizeof g_image, written);
    // Nothing is written to flash before the whole image is known to fit, so
    // a patch too large for a slot leaves what was there intact.
    if (e != CODEC_OK) return error = (e == CODEC_TOO_MANY_NODES ? STORE_CORRUPT : STORE_TOO_LARGE);

    const size_t base = slot_base(slot);
    if (base + written > mem.size()) return error = STORE_TOO_LARGE;
    for (size_t i = 0; i < written; i++) mem.write(base + i, g_image[i]);
    write_count++;
    if (slot == 0){ is_dirty = false; }
    return error = STORE_OK;
}

StoreError PatchStore::load(uint8_t slot, Patch& patch, GlobalSettings& globals) const {
    if (slot >= PATCH_SLOTS) return STORE_NO_SUCH_SLOT;

    const size_t base = slot_base(slot);
    // Read the header first so a short image is not copied in full.
    uint8_t header[8];
    for (size_t i = 0; i < sizeof header; i++) header[i] = mem.read(base + i);
    const uint32_t magic = (uint32_t)header[0] | ((uint32_t)header[1] << 8)
                         | ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    // A freshly flashed module reads 0xFF everywhere; that is empty, not
    // corrupt, and the difference matters to what the red LED says.
    if (magic == 0xFFFFFFFFu || magic == 0) return STORE_EMPTY;
    if (magic != PATCH_MAGIC) return STORE_CORRUPT;

    const uint16_t payload = (uint16_t)(header[6] | ((uint16_t)header[7] << 8));
    const size_t total = (size_t)8u + payload + 2u;
    if (total > sizeof g_image || base + total > mem.size()) return STORE_CORRUPT;

    for (size_t i = 0; i < total; i++) g_image[i] = mem.read(base + i);

    // Decoded into the caller's buffers only after the CRC has checked out
    // (patch_codec::decode does that first), so a corrupt slot never
    // half-fills a patch.
    Patch decoded;
    GlobalSettings decoded_globals;
    const CodecError e = patch_codec::decode(g_image, total, decoded, decoded_globals);
    if (e != CODEC_OK) return STORE_CORRUPT;

    patch = decoded;
    globals = decoded_globals;
    return STORE_OK;
}

bool PatchStore::occupied(uint8_t slot) const {
    Patch p;
    GlobalSettings g;
    return load(slot, p, g) == STORE_OK;
}

uint16_t PatchStore::used(uint8_t slot) const {
    if (!occupied(slot)) return 0;
    const size_t base = slot_base(slot);
    const uint16_t payload = (uint16_t)(mem.read(base + 6) | ((uint16_t)mem.read(base + 7) << 8));
    return (uint16_t)(8u + payload + 2u);
}

void PatchStore::erase(uint8_t slot){
    if (slot >= PATCH_SLOTS) return;
    // Only the magic has to go: an image whose magic does not check out is
    // never read further, and clearing four bytes costs four write cycles
    // instead of a kilobyte of them.
    const size_t base = slot_base(slot);
    for (size_t i = 0; i < 4; i++) mem.write(base + i, 0xFF);
    write_count++;
    if (slot == 0) is_dirty = false;
}

void PatchStore::mark_dirty(uint32_t now_us){
    // The timer starts on the first edit of a burst and is not restarted by
    // the ones that follow: a controller that never stops moving would
    // otherwise postpone the save for as long as it kept moving.
    if (!is_dirty){
        is_dirty = true;
        dirty_since_us = now_us;
    }
}

bool PatchStore::service(uint32_t now_us, const Patch& patch, const GlobalSettings& globals){
    if (!is_dirty) return false;
    if ((uint32_t)(now_us - dirty_since_us) < AUTOSAVE_SETTLE_US) return false;
    save(0, patch, globals);
    return true;
}
