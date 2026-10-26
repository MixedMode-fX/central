#ifndef VST_HOST_SAMPLE_FORMAT_H
#define VST_HOST_SAMPLE_FORMAT_H

#include <stdint.h>

// Between the Audio Library's 16-bit blocks and the float buffers a plug-in
// processes: full scale is 1.0, and a float beyond it clips to the 16-bit
// limits on the way back rather than wrapping. Framework-free.

namespace vst_host {

inline void toFloat(const int16_t* in, float* out, int frames)
{
    const float scale = 1.0f / 32768.0f;
    for (int i = 0; i < frames; i++) out[i] = static_cast<float>(in[i]) * scale;
}

inline void silence(float* out, int frames)
{
    for (int i = 0; i < frames; i++) out[i] = 0.0f;
}

inline int16_t toSample(float x)
{
    const float scaled = x * 32768.0f;
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    // Round to nearest; the truncation toward zero of a plain cast would
    // bias quiet signals.
    return static_cast<int16_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

inline void toInt16(const float* in, int16_t* out, int frames)
{
    for (int i = 0; i < frames; i++) out[i] = toSample(in[i]);
}

}  // namespace vst_host

#endif
