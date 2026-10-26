#if defined(__MK66FX1M0__) || defined(__IMXRT1052__) || defined(__IMXRT1062__)

#include "vst_host/audio_effect_vst.h"
#include "vst_host/sample_format.h"

static const int CHANNELS = VSTEffect::MAX_CHANNELS;

// Three blocks of float buffers: a block each way for one plug-in, or the
// input, output and parallel sum of a rack's stage. Updates run one at a
// time in the Audio Library's software interrupt, so every host object can
// share them.
static float scratch_a[CHANNELS][AUDIO_BLOCK_SAMPLES];
static float scratch_b[CHANNELS][AUDIO_BLOCK_SAMPLES];
static float scratch_c[CHANNELS][AUDIO_BLOCK_SAMPLES];

static unsigned char clampInputs(const AudioEffectX& plugin)
{
    const VstInt32 n = plugin.getNumInputs();
    if (n <= 0) return 0;
    return n > CHANNELS ? CHANNELS : static_cast<unsigned char>(n);
}

// The input blocks as float; a missing block is silence.
static void blocksToFloat(audio_block_t* const* in, float** out)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (in[ch]) {
            vst_host::toFloat(in[ch]->data, out[ch], AUDIO_BLOCK_SAMPLES);
        } else {
            vst_host::silence(out[ch], AUDIO_BLOCK_SAMPLES);
        }
    }
}

static void printEffect(Print& out, VSTEffect& effect, int stage)
{
    char text[VSTEffect::TEXT_LEN];
    effect.effectName(text);
    out.print(text);
    if (effect.bypassed()) out.print(" (bypassed)");
    if (!effect.empty() && !effect.supported()) out.print(" (unsupported channel count, passed through)");
    if (stage >= 0) {
        out.print("  stage ");
        out.print(stage);
    }
    if (effect.gain() != 1.0f) {
        out.print("  gain ");
        out.print(effect.gain(), 2);
    }
    if (effect.mix() != 1.0f) {
        out.print("  mix ");
        out.print(effect.mix(), 2);
    }
    out.println();
    for (int i = 0; i < effect.numParams(); i++) {
        out.print("  [");
        out.print(i);
        out.print("] ");
        effect.parameterName(i, text);
        out.print(text);
        for (int pad = static_cast<int>(strlen(text)); pad < kVstMaxParamStrLen + 1; pad++) out.print(' ');
        out.print(effect.getParameter(i), 3);
        out.print("  ");
        effect.parameterDisplay(i, text);
        out.print(text);
        effect.parameterLabel(i, text);
        if (text[0] != 0 && text[0] != ' ') {
            out.print(' ');
            out.print(text);
        }
        out.println();
    }
}

static float percentOfProcessor(uint32_t cycles)
{
#if defined(F_CPU_ACTUAL)
    const float clock = static_cast<float>(F_CPU_ACTUAL);
#else
    const float clock = static_cast<float>(F_CPU);
#endif
    return static_cast<float>(cycles) * (AUDIO_SAMPLE_RATE_EXACT / AUDIO_BLOCK_SAMPLES) * 100.0f / clock;
}

static uint32_t readCycleCounter()
{
    return ARM_DWT_CYCCNT;
}

/* ------------------------------------------------------------------------- */
/* One plug-in                                                               */
/* ------------------------------------------------------------------------- */

AudioEffectVSTHost::AudioEffectVSTHost(AudioEffectX& plugin)
    : AudioStream(clampInputs(plugin), inputQueue_), VSTEffect(plugin), inputQueue_()
{
    plugin.setSampleRate(AUDIO_SAMPLE_RATE_EXACT);
    plugin.setBlockSize(AUDIO_BLOCK_SAMPLES);
    plugin.resume();
}

void AudioEffectVSTHost::printParameters(Print& out)
{
    printEffect(out, *this, -1);
}

void AudioEffectVSTHost::update(void)
{
    audio_block_t* in[CHANNELS];
    for (int ch = 0; ch < CHANNELS; ch++) in[ch] = receiveReadOnly(ch);

    if (bypassed() || !supported()) {
        for (int ch = 0; ch < CHANNELS; ch++) {
            if (!in[ch]) continue;
            transmit(in[ch], ch);
            release(in[ch]);
        }
        return;
    }

    float* ins[CHANNELS];
    float* outs[CHANNELS];
    for (int ch = 0; ch < CHANNELS; ch++) {
        ins[ch] = scratch_a[ch];
        outs[ch] = scratch_b[ch];
    }
    blocksToFloat(in, ins);
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (in[ch]) release(in[ch]);
    }
    process(ins, outs, AUDIO_BLOCK_SAMPLES);

    for (int ch = 0; ch < CHANNELS; ch++) {
        audio_block_t* block = allocate();
        if (!block) return;
        vst_host::toInt16(outs[ch], block->data, AUDIO_BLOCK_SAMPLES);
        transmit(block, ch);
        release(block);
    }
}

/* ------------------------------------------------------------------------- */
/* A rack                                                                    */
/* ------------------------------------------------------------------------- */

AudioEffectVSTChain::AudioEffectVSTChain() : AudioStream(CHANNELS, inputQueue_), VSTRack(), inputQueue_()
{
    sampleRate(AUDIO_SAMPLE_RATE_EXACT);
    blockSize(AUDIO_BLOCK_SAMPLES);
    clock(readCycleCounter);
}

float AudioEffectVSTChain::processorUsage(int n) const
{
    return percentOfProcessor(cycles(n));
}

float AudioEffectVSTChain::processorUsageMax(int n) const
{
    return percentOfProcessor(cyclesMax(n));
}

void AudioEffectVSTChain::printParameters(Print& out)
{
    for (int n = 0; n < size(); n++) {
        out.print(n);
        out.print(": ");
        printEffect(out, *slot(n), stageOf(n));
    }
}

void AudioEffectVSTChain::update(void)
{
    audio_block_t* in[CHANNELS];
    for (int ch = 0; ch < CHANNELS; ch++) in[ch] = receiveReadOnly(ch);

    if (size() == 0) {
        for (int ch = 0; ch < CHANNELS; ch++) {
            if (!in[ch]) continue;
            transmit(in[ch], ch);
            release(in[ch]);
        }
        return;
    }

    float* current[CHANNELS];
    float* next[CHANNELS];
    float* spare[CHANNELS];
    for (int ch = 0; ch < CHANNELS; ch++) {
        current[ch] = scratch_a[ch];
        next[ch] = scratch_b[ch];
        spare[ch] = scratch_c[ch];
    }
    blocksToFloat(in, current);
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (in[ch]) release(in[ch]);
    }

    float** result = process(current, next, spare, AUDIO_BLOCK_SAMPLES);

    for (int ch = 0; ch < CHANNELS; ch++) {
        audio_block_t* block = allocate();
        if (!block) return;
        vst_host::toInt16(result[ch], block->data, AUDIO_BLOCK_SAMPLES);
        transmit(block, ch);
        release(block);
    }
}

#endif
