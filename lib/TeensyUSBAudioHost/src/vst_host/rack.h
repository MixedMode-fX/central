#ifndef VST_HOST_RACK_H
#define VST_HOST_RACK_H

#include <stdint.h>
#include "vst_host/effect.h"

// Slots in a rack. Overridable from the build.
#ifndef VST_HOST_MAX_CHAIN
#define VST_HOST_MAX_CHAIN 8
#endif

// A row of effects in stages, processed on float buffers. Each stage holds
// one or more slots that all read the stage's input; their outputs, each at
// its own gain(), are summed into the stage's output, which is the next
// stage's input. A stage of one slot is a plug-in in series; a stage of
// several is plug-ins in parallel; a slot with no plug-in is a wire, which
// is how the dry signal joins a sum:
//
//     rack.add(filter);                       // stage 0
//     rack.add(compressor)->gain(0.7f);       // stage 1: the compressed signal...
//     rack.addParallel()->gain(0.5f);         //   ...plus the dry one, in parallel
//     rack.add(tape)->mix(0.5f);              // stage 2, half wet
//
// Slots are numbered in the order they were added, across stages; slot(n)
// is the effect's controls, stageOf(n) says where it sits. Add from the
// main program while audio runs if you like: a slot is complete before it
// is counted. Framework-free; AudioEffectVSTChain is this with the Audio
// Library around it, and reports each slot's cycles if given a clock.
class VSTRack {
public:
    static const int MAX_SLOTS = VST_HOST_MAX_CHAIN;
    static const int MAX_CHANNELS = VSTEffect::MAX_CHANNELS;
    typedef uint32_t (*Clock)();

    VSTRack()
        : slots_(), stage_(), size_(0), rate_(44100.0f), block_(128), clock_(nullptr), cycles_(), cycles_max_()
    {
    }
    VSTRack(const VSTRack&) = delete;
    VSTRack& operator=(const VSTRack&) = delete;

    // A plug-in as a new stage after the last, or in parallel with the
    // last stage's slots. The no-argument forms add a wire. Each returns
    // the slot, or nullptr when the rack is full.
    VSTEffect* add(AudioEffectX& plugin) { return addSlot(&plugin, false); }
    VSTEffect* add() { return addSlot(nullptr, false); }
    VSTEffect* addParallel(AudioEffectX& plugin) { return addSlot(&plugin, true); }
    VSTEffect* addParallel() { return addSlot(nullptr, true); }

    int size() const { return size_; }
    int stages() const { return size_ ? stage_[size_ - 1] + 1 : 0; }
    int stageOf(int n) const { return n >= 0 && n < size_ ? stage_[n] : -1; }
    VSTEffect* slot(int n) { return n >= 0 && n < size_ ? &slots_[n] : nullptr; }

    // What every plug-in is told, present and future.
    void sampleRate(float hz)
    {
        rate_ = hz;
        for (int n = 0; n < size_; n++) slots_[n].sampleRate(hz);
    }
    float sampleRate() const { return rate_; }
    void blockSize(int frames) { block_ = frames; }

    // Per-slot timing, in whatever the clock counts; nothing without one.
    void clock(Clock read) { clock_ = read; }
    uint32_t cycles(int n) const { return n >= 0 && n < size_ ? cycles_[n] : 0; }
    uint32_t cyclesMax(int n) const { return n >= 0 && n < size_ ? cycles_max_[n] : 0; }
    void resetCyclesMax()
    {
        for (int n = 0; n < MAX_SLOTS; n++) cycles_max_[n] = cycles_[n];
    }

    // Runs frames from `current` through every stage. `next` and `spare`
    // are work buffers of MAX_CHANNELS by frames; all three are distinct.
    // Returns the buffer holding the result, which is `current` itself for
    // an empty rack.
    float** process(float** current, float** next, float** spare, int frames)
    {
        const int count = size_;
        int first = 0;
        while (first < count) {
            const int stage = stage_[first];
            int end = first;
            while (end < count && stage_[end] == stage) end++;
            for (int n = first; n < end; n++) {
                float** out = n == first ? next : spare;
                const uint32_t start = clock_ ? clock_() : 0;
                slots_[n].process(current, out, frames);
                if (clock_) {
                    const uint32_t took = clock_() - start;
                    cycles_[n] = took;
                    if (took > cycles_max_[n]) cycles_max_[n] = took;
                }
                if (n != first) {
                    for (int ch = 0; ch < MAX_CHANNELS; ch++)
                        for (int i = 0; i < frames; i++) next[ch][i] += spare[ch][i];
                }
            }
            float** done = next;
            next = current;
            current = done;
            first = end;
        }
        return current;
    }

private:
    VSTEffect* addSlot(AudioEffectX* plugin, bool parallel)
    {
        const int n = size_;
        if (n >= MAX_SLOTS) return nullptr;
        VSTEffect& slot = slots_[n];
        if (plugin) {
            slot.attach(*plugin);
            plugin->setSampleRate(rate_);
            plugin->setBlockSize(block_);
            plugin->resume();
        }
        stage_[n] = n == 0 ? 0 : stage_[n - 1] + (parallel ? 0 : 1);
        cycles_[n] = 0;
        cycles_max_[n] = 0;
        // The slot is complete before anything processing can see it.
        size_ = n + 1;
        return &slot;
    }

    VSTEffect slots_[MAX_SLOTS];
    uint8_t stage_[MAX_SLOTS];
    volatile int size_;
    float rate_;
    int block_;
    Clock clock_;
    uint32_t cycles_[MAX_SLOTS];
    uint32_t cycles_max_[MAX_SLOTS];
};

#endif
