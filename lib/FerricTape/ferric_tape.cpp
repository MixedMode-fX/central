#include "ferric_tape.h"
#include <math.h>
#include <string.h>

namespace {

const char* const NAMES[FerricTape::NUM_PARAMS] = {"Drive", "Comp",   "Bits", "Tone",     "Wow",  "Flutter",
                                                   "Dropout", "Chorus", "Time", "Feedback", "Slap", "Mix"};

// A tape pushed a little, a little worn, with a short dark slap.
const float DEFAULTS[FerricTape::NUM_PARAMS] = {0.25f, 0.3f, 0.3f,  0.45f, 0.3f, 0.25f,
                                                0.15f, 0.25f, 0.35f, 0.35f, 0.3f, 1.0f};

const float PI_F = 3.14159265f;

// Clouds' FORMAT_12_BIT: x * 4096 in a 16-bit word, saturating.
inline uint16_t compress(float x)
{
    int32_t v = static_cast<int32_t>(x * 4096.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return static_cast<uint16_t>(v);
}

inline float decompress(uint16_t v)
{
    return static_cast<float>(static_cast<int16_t>(v)) / 4096.0f;
}

// The sample `delay` samples before the one at write, delay >= 0,
// interpolated linearly. size is a power of two.
inline float tap(const uint16_t* line, uint32_t size, uint32_t write, float delay)
{
    const uint32_t whole = static_cast<uint32_t>(delay);
    const float frac = delay - static_cast<float>(whole);
    const float a = decompress(line[(write - whole) & (size - 1)]);
    const float b = decompress(line[(write - whole - 1) & (size - 1)]);
    return a + (b - a) * frac;
}

// arctan to within 0.0015 rad (Rajan et al.), folding |x| > 1 over
// atan(x) = pi/2 - atan(1/x).
inline float arctan(float x)
{
    const float a = fabsf(x);
    if (a <= 1.0f) return x * (0.7853982f - (a - 1.0f) * (0.2447f + 0.0663f * a));
    const float r = 1.0f / x;
    const float ar = 1.0f / a;
    const float inner = r * (0.7853982f - (ar - 1.0f) * (0.2447f + 0.0663f * ar));
    return (x > 0.0f ? 0.5f * PI_F : -0.5f * PI_F) - inner;
}

// sin(2 pi phase) for phase in 0..1, to within 0.001: a parabola, refined.
inline float sine(float phase)
{
    const float t = 2.0f * phase - 1.0f;  // -1..1, a half-turn off
    const float y = -4.0f * t + 4.0f * t * fabsf(t);
    return 0.225f * (y * fabsf(y) - y) + y;
}

inline float wrap(float phase)
{
    return phase >= 1.0f ? phase - 1.0f : phase;
}

// A one-pole's coefficient for a corner at hz.
inline float onePole(float hz, float rate)
{
    const float a = 1.0f - expf(-2.0f * PI_F * hz / rate);
    return a > 1.0f ? 1.0f : a;
}

inline float clamp(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

}  // namespace

FerricTape::FerricTape(audioMasterCallback master)
    : AudioEffectX(master, 0, NUM_PARAMS),
      params_(),
      clear_(false),
      write_(0),
      noise_(0x2545f491u),
      wow_phase_(0.0f),
      flutter_phase_(0.0f),
      chorus_phase_(0.0f),
      drift_(0.0f),
      drift_mid_(0.0f),
      drift_target_(0.0f),
      drift_left_(0),
      scrape_(0.0f),
      dropout_gain_(1.0f),
      dropout_target_(1.0f),
      dropout_left_(0),
      envelope_(0.0f),
      slap_delay_(-1.0f),
      ch_(),
      tape_(),
      slap_()
{
    setNumInputs(2);
    setNumOutputs(2);
    setUniqueID(0x66657272);  // 'ferr'
    canProcessReplacing();
    for (int i = 0; i < NUM_PARAMS; i++) params_[i] = DEFAULTS[i];
}

void FerricTape::reset()
{
    memset(tape_, 0, sizeof tape_);
    memset(slap_, 0, sizeof slap_);
    memset(ch_, 0, sizeof ch_);
    envelope_ = 0.0f;
}

float FerricTape::slapMs(float value) const
{
    float longest = 1000.0f * static_cast<float>(SLAP_WORDS - 4) / getSampleRate();
    if (longest > 350.0f) longest = 350.0f;
    return 40.0f * powf(longest / 40.0f, value);
}

void FerricTape::setParameter(VstInt32 index, float value)
{
    if (index < 0 || index >= NUM_PARAMS) return;
    params_[index] = clamp(value, 0.0f, 1.0f);
}

float FerricTape::getParameter(VstInt32 index)
{
    return index >= 0 && index < NUM_PARAMS ? params_[index] : 0.0f;
}

void FerricTape::getParameterName(VstInt32 index, char* text)
{
    text[0] = 0;
    if (index >= 0 && index < NUM_PARAMS) vst_strncpy(text, NAMES[index], kVstMaxParamStrLen);
}

void FerricTape::getParameterDisplay(VstInt32 index, char* text)
{
    text[0] = 0;
    if (index < 0 || index >= NUM_PARAMS) return;
    const float v = params_[index];
    switch (index) {
    case PARAM_DRIVE:
        int2string(static_cast<VstInt32>(24.0f * v + 0.5f), text, kVstMaxParamStrLen);
        break;
    case PARAM_COMP:
        float2string(1.0f + 5.0f * v, text, 3);
        break;
    case PARAM_BITS:
        float2string(16.0f - 12.0f * v, text, 4);
        break;
    case PARAM_TONE:
        float2string(1.2f * powf(16.0f / 1.2f, v), text, 4);
        break;
    case PARAM_TIME:
        int2string(static_cast<VstInt32>(slapMs(v) + 0.5f), text, kVstMaxParamStrLen);
        break;
    default:
        int2string(static_cast<VstInt32>(100.0f * v + 0.5f), text, kVstMaxParamStrLen);
        break;
    }
}

void FerricTape::getParameterLabel(VstInt32 index, char* text)
{
    const char* label = "%";
    if (index == PARAM_DRIVE) label = "dB";
    else if (index == PARAM_COMP) label = ":1";
    else if (index == PARAM_BITS) label = "bits";
    else if (index == PARAM_TONE) label = "kHz";
    else if (index == PARAM_TIME) label = "ms";
    else if (index < 0 || index >= NUM_PARAMS) label = "";
    vst_strncpy(text, label, kVstMaxParamStrLen);
}

bool FerricTape::getEffectName(char* name)
{
    vst_strncpy(name, "Ferric", kVstMaxEffectNameLen);
    return true;
}

bool FerricTape::getVendorString(char* text)
{
    vst_strncpy(text, "MixedMode fX", kVstMaxVendorStrLen);
    return true;
}

bool FerricTape::getProductString(char* text)
{
    vst_strncpy(text, "Ferric tape", kVstMaxProductStrLen);
    return true;
}

void FerricTape::processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames)
{
    if (clear_) {
        clear_ = false;
        reset();
    }

    // Everything a parameter or the rate sets, once a block, as loop() may
    // be moving the parameters.
    const float rate = getSampleRate();
    const float* p = params_;

    const float threshold = 0.25f;
    const float ratio = 1.0f + 5.0f * p[PARAM_COMP];
    const float at_full_scale = threshold + (1.0f - threshold) / ratio;
    const float makeup = 1.0f + 0.5f * (1.0f / at_full_scale - 1.0f);
    const float attack = onePole(1.0f / (2.0f * PI_F * 0.010f), rate);
    const float release = onePole(1.0f / (2.0f * PI_F * 0.150f), rate);

    const float drive = powf(10.0f, 1.2f * p[PARAM_DRIVE]);  // 0..24 dB
    const float drive_norm = 1.0f / arctan(drive);
    const float levels = powf(2.0f, 15.0f - 12.0f * p[PARAM_BITS]);

    // Squared, so the bottom of each knob is fine and the top is wrecked.
    const float wow_depth = 0.005f * rate * p[PARAM_WOW] * p[PARAM_WOW];
    const float flutter_depth = 0.0003f * rate * p[PARAM_FLUTTER] * p[PARAM_FLUTTER];
    const float wow_step = 0.55f / rate;
    const float flutter_step = 7.5f / rate;
    const float drift_glide = onePole(1.5f, rate);
    const uint32_t drift_hold = static_cast<uint32_t>(0.25f * rate);
    const float scrape_k = onePole(30.0f, rate);
    const float scrape_gain = 0.9f / sqrtf(scrape_k);
    const float tape_delay = 0.0065f * rate;  // the playback head, past the deepest wobble's 5.3 ms

    const float chorus = p[PARAM_CHORUS];
    const float chorus_depth = 0.004f * rate * chorus;
    const float chorus_delay = tape_delay + 0.008f * rate;
    const float chorus_step = (0.3f + 0.9f * chorus) / rate;
    const float chorus_level = 0.7f * chorus;
    const float chorus_dry = 1.0f - 0.3f * chorus;

    const float dropout = p[PARAM_DROPOUT];
    const float dropout_chance = 4.0f * dropout * dropout / rate;
    const float dropout_glide = onePole(1.0f / (2.0f * PI_F * 0.005f), rate);

    const float tone = onePole(1200.0f * powf(16000.0f / 1200.0f, p[PARAM_TONE]), rate);
    const float hp = onePole(30.0f, rate);
    const float fb_lp = onePole(600.0f * powf(16000.0f / 1200.0f, p[PARAM_TONE]), rate);
    const float fb_hp = onePole(80.0f, rate);

    const float tape_limit = static_cast<float>(TAPE_WORDS - 2);
    const float slap_limit = static_cast<float>(SLAP_WORDS - 2);
    const float slap_target = clamp(slapMs(p[PARAM_TIME]) * 0.001f * rate, 1.0f, slap_limit);
    if (slap_delay_ < 0.0f) slap_delay_ = slap_target;
    const float slap_glide = onePole(4.0f, rate);
    const float feedback = p[PARAM_FEEDBACK];
    const float slap_level = p[PARAM_SLAP];
    const float mix = p[PARAM_MIX];

    for (VstInt32 i = 0; i < sampleFrames; i++) {
        const float dry[2] = {inputs[0][i], inputs[1][i]};

        // The compressor, linked: one gain from the louder side.
        const float level = fabsf(dry[0]) > fabsf(dry[1]) ? fabsf(dry[0]) : fabsf(dry[1]);
        envelope_ += (level - envelope_) * (level > envelope_ ? attack : release);
        float gain = makeup;
        if (envelope_ > threshold) gain *= (threshold + (envelope_ - threshold) / ratio) / envelope_;

        // The transport: one motor for both sides.
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        const float white = static_cast<float>(static_cast<int32_t>(noise_)) * (1.0f / 2147483648.0f);
        if (drift_left_ == 0) {
            drift_left_ = drift_hold;
            drift_target_ = white;
        }
        drift_left_--;
        drift_mid_ += (drift_target_ - drift_mid_) * drift_glide;
        drift_ += (drift_mid_ - drift_) * drift_glide;
        scrape_ += (white - scrape_) * scrape_k;
        wow_phase_ = wrap(wow_phase_ + wow_step);
        flutter_phase_ = wrap(flutter_phase_ + flutter_step);
        chorus_phase_ = wrap(chorus_phase_ + chorus_step);
        // Each part kept within its depth: the head stays behind the write
        // point, and the chorus head, 12 ms further back, stays on the tape.
        const float wobble = wow_depth * (0.6f * sine(wow_phase_) + 0.4f * drift_) +
                             flutter_depth * (0.6f * sine(flutter_phase_) + 0.4f * clamp(scrape_gain * scrape_, -1.0f, 1.0f));
        const float head = clamp(tape_delay + wobble, 0.0f, tape_limit);

        // Dropouts: at random, a dip of random depth and length.
        if (dropout_left_ > 0) {
            if (--dropout_left_ == 0) dropout_target_ = 1.0f;
        } else if (static_cast<float>(noise_ >> 8) * (1.0f / 16777216.0f) < dropout_chance) {
            const float a = static_cast<float>((noise_ >> 4) & 0xff) * (1.0f / 255.0f);
            const float b = static_cast<float>((noise_ >> 12) & 0xff) * (1.0f / 255.0f);
            dropout_left_ = static_cast<uint32_t>((0.02f + 0.18f * a) * rate) + 1;
            dropout_target_ = 1.0f - 0.95f * dropout * (0.3f + 0.7f * b);
        }
        dropout_gain_ += (dropout_target_ - dropout_gain_) * dropout_glide;
        const float dull = tone * (0.15f + 0.85f * dropout_gain_ * dropout_gain_);

        slap_delay_ += (slap_target - slap_delay_) * slap_glide;
        const float slap_head = clamp(slap_delay_ + wobble, 1.0f, slap_limit);

        for (int c = 0; c < 2; c++) {
            Channel& s = ch_[c];

            // Onto the tape: squeezed, driven, rounded.
            float x = arctan(dry[c] * gain * drive) * drive_norm;
            x = floorf(x * levels + 0.5f) / levels;
            uint16_t* tape = tape_[c];
            tape[write_ & (TAPE_WORDS - 1)] = compress(x);

            // Off it, through the wobbling heads.
            float y = tap(tape, TAPE_WORDS, write_, head);
            if (chorus > 0.0f) {
                const float sweep = sine(c ? wrap(chorus_phase_ + 0.25f) : chorus_phase_);
                const float voice = tap(tape, TAPE_WORDS, write_, clamp(chorus_delay + wobble + chorus_depth * sweep, 0.0f, tape_limit));
                y = chorus_dry * y + chorus_level * voice;
            }

            y *= dropout_gain_;
            s.hp += (y - s.hp) * hp;
            y -= s.hp;
            s.lp1 += (y - s.lp1) * dull;
            s.lp2 += (s.lp1 - s.lp2) * dull;
            y = s.lp2;

            // The slap: its repeats darkened, then saturated back in.
            uint16_t* slap = slap_[c];
            const float echo = tap(slap, SLAP_WORDS, write_, slap_head);
            s.fb_lp += (echo - s.fb_lp) * fb_lp;
            s.fb_hp += (s.fb_lp - s.fb_hp) * fb_hp;
            slap[write_ & (SLAP_WORDS - 1)] = compress(y + arctan(feedback * (s.fb_lp - s.fb_hp)));

            const float wet = y + slap_level * echo;
            outputs[c][i] = dry[c] + mix * (wet - dry[c]);
        }
        write_++;
    }
}
