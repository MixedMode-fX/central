#if defined(__MK66FX1M0__) || defined(__IMXRT1052__) || defined(__IMXRT1062__)

#include "usb_audio_host/audio_nodes.h"

// One block of interleaved frames. Updates run one at a time in the audio
// library's software interrupt, so both objects can share it.
static int32_t scratch[AUDIO_BLOCK_SAMPLES * USB_AUDIO_HOST_MAX_CHANNELS];

// Whether a USB host object holds the Audio Library's update responsibility.
// Shared, because the device calls back whichever object registered last
// and the input always wins that registration: an output that claimed the
// responsibility first must still see the graph driven.
static bool drives_updates = false;

static uint8_t clampChannels(uint8_t n)
{
    if (n == 0) return 1;
    return n > USB_AUDIO_HOST_MAX_CHANNELS ? USB_AUDIO_HOST_MAX_CHANNELS : n;
}

// Latency is bounded here rather than in the device: if the graph falls
// behind, frames older than two blocks are discarded before the read.
static const uint32_t MAX_BACKLOG_BLOCKS = 3;

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

AudioInputUSBHost::AudioInputUSBHost(USBAudioDevice& device, uint8_t channels)
    : AudioStream(0, nullptr), device_(device), channels_(clampChannels(channels))
{
}

void AudioInputUSBHost::begin()
{
    update_responsibility_ = update_setup();
    if (update_responsibility_) drives_updates = true;
    device_.onBlock(onBlock, this, AUDIO_BLOCK_SAMPLES);
}

void AudioInputUSBHost::onBlock(void*)
{
    if (drives_updates) AudioStream::update_all();
}

void AudioInputUSBHost::update(void)
{
    device_.blockTaken();
    if (!device_.streaming()) return;
    const uint8_t device_channels = device_.inputChannels();
    if (device_channels == 0) return;
    const uint32_t available = device_.inputAvailable();
    if (available > MAX_BACKLOG_BLOCKS * AUDIO_BLOCK_SAMPLES) {
        dropped_ += device_.discardInput(available - 2 * AUDIO_BLOCK_SAMPLES);
    } else if (available < AUDIO_BLOCK_SAMPLES) {
        underruns_++;
    }
    device_.readInput(scratch, AUDIO_BLOCK_SAMPLES);
    const uint8_t n = channels_ < device_channels ? channels_ : device_channels;
    for (uint8_t ch = 0; ch < n; ch++) {
        audio_block_t* block = allocate();
        if (!block) return;
        const int32_t* src = scratch + ch;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            block->data[i] = static_cast<int16_t>(*src >> 16);
            src += device_channels;
        }
        transmit(block, ch);
        release(block);
    }
}

/* ------------------------------------------------------------------------- */
/* Output                                                                    */
/* ------------------------------------------------------------------------- */

AudioOutputUSBHost::AudioOutputUSBHost(USBAudioDevice& device, uint8_t channels)
    : AudioStream(clampChannels(channels), queue_), device_(device), channels_(clampChannels(channels))
{
}

// The input object, when there is one, is the clock; the output only asks
// for blocks when no input object registered, and the device only calls it
// when the interface has no input stream to pace by.
void AudioOutputUSBHost::begin()
{
    update_responsibility_ = update_setup();
    if (update_responsibility_) drives_updates = true;
    if (!device_.hasBlockListener()) device_.onBlock(onBlock, this, AUDIO_BLOCK_SAMPLES);
}

void AudioOutputUSBHost::onBlock(void*)
{
    if (drives_updates) AudioStream::update_all();
}

void AudioOutputUSBHost::update(void)
{
    device_.blockTaken();
    const uint8_t device_channels = device_.outputChannels();
    const bool live = device_.streaming() && device_channels > 0;
    const uint8_t n = channels_ < device_channels ? channels_ : device_channels;
    if (live) {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES * device_channels; i++) scratch[i] = 0;
    }
    for (uint8_t ch = 0; ch < channels_; ch++) {
        audio_block_t* block = receiveReadOnly(ch);
        if (!block) continue;
        if (live && ch < n) {
            int32_t* dst = scratch + ch;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                *dst = static_cast<int32_t>(block->data[i]) * 65536;
                dst += device_channels;
            }
        }
        release(block);
    }
    if (live) device_.writeOutput(scratch, AUDIO_BLOCK_SAMPLES);
}

#endif
