#include "usb_audio_host/audio_function.h"
#include "usb_audio_host/uac.h"

namespace usb_audio_host {

static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static uint32_t le24(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)); }

bool Format::supportsRate(uint32_t rate) const
{
    if (rate == 0) return true; // any rate: the caller will take the format's own
    if (n_rates == 0 && rate_max == 0) return true; // UAC2: decided by the clock, not the format
    if (n_rates == 0) return rate >= rate_min && rate <= rate_max;
    for (uint8_t i = 0; i < n_rates; i++) {
        if (rates[i] == rate) return true;
    }
    return false;
}

bool ClockEntity::programmable() const
{
    return subtype == uac::AC_CLOCK_SOURCE &&
           (controls & uac::CLOCK_FREQ_CONTROL_MASK) == uac::CLOCK_FREQ_PROGRAMMABLE;
}

void AudioFunction::clear()
{
    *this = AudioFunction();
}

bool AudioFunction::parse(const uint8_t* d, uint32_t length)
{
    clear();
    if (length < 9 || d[0] != 9 || d[1] != uac::DT_INTERFACE) return false;
    if (d[5] != uac::CLASS_AUDIO || d[6] != uac::SUBCLASS_AUDIOCONTROL) return false;
    control_interface_ = d[2];
    version_ = (d[7] == uac::PROTOCOL_UAC2) ? UacVersion::V2 : UacVersion::V1;

    enum Section { CONTROL, STREAMING, OTHER } section = CONTROL;
    Format pending;
    const uint8_t* end = d + length;
    d += 9;
    while (d + 2 <= end) {
        const uint8_t len = d[0];
        const uint8_t type = d[1];
        if (len < 2 || d + len > end) break;
        if (type == uac::DT_INTERFACE && len >= 9) {
            commit(pending);
            const bool audio = d[5] == uac::CLASS_AUDIO;
            if (audio && d[6] == uac::SUBCLASS_AUDIOSTREAMING) {
                section = STREAMING;
                pending.interface = d[2];
                pending.alt = d[3];
            } else if (audio && d[6] == uac::SUBCLASS_MIDISTREAMING) {
                // MIDI rides along inside the same function; not ours.
                section = OTHER;
            } else {
                // A second control interface or a foreign class: this
                // function ends here.
                break;
            }
        } else if (type == uac::DT_IAD) {
            break;
        } else if (section == CONTROL && type == uac::DT_CS_INTERFACE) {
            parseControl(d);
        } else if (section == STREAMING) {
            parseStreaming(d, pending);
        }
        d += len;
    }
    commit(pending);
    return n_formats_ > 0;
}

void AudioFunction::parseControl(const uint8_t* d)
{
    const uint8_t len = d[0];
    const uint8_t subtype = d[2];
    const bool v2 = version_ == UacVersion::V2;
    switch (subtype) {
    case uac::AC_INPUT_TERMINAL:
        if (n_terminals_ >= USB_AUDIO_HOST_MAX_ENTITIES) return;
        if (v2 && len >= 17) {
            Terminal& t = terminals_[n_terminals_++];
            t.id = d[3]; t.type = le16(d + 4); t.input = true;
            t.clock_id = d[7]; t.channels = d[8];
        } else if (!v2 && len >= 12) {
            Terminal& t = terminals_[n_terminals_++];
            t.id = d[3]; t.type = le16(d + 4); t.input = true;
            t.channels = d[7];
        }
        return;
    case uac::AC_OUTPUT_TERMINAL:
        if (n_terminals_ >= USB_AUDIO_HOST_MAX_ENTITIES) return;
        if (v2 && len >= 12) {
            Terminal& t = terminals_[n_terminals_++];
            t.id = d[3]; t.type = le16(d + 4); t.input = false;
            t.clock_id = d[8];
        } else if (!v2 && len >= 9) {
            Terminal& t = terminals_[n_terminals_++];
            t.id = d[3]; t.type = le16(d + 4); t.input = false;
        }
        return;
    case uac::AC_CLOCK_SOURCE:
        if (!v2 || len < 8 || n_clocks_ >= USB_AUDIO_HOST_MAX_ENTITIES) return;
        {
            ClockEntity& c = clocks_[n_clocks_++];
            c.id = d[3]; c.subtype = subtype; c.attributes = d[4]; c.controls = d[5];
        }
        return;
    case uac::AC_CLOCK_SELECTOR:
        if (!v2 || len < 7 || n_clocks_ >= USB_AUDIO_HOST_MAX_ENTITIES) return;
        {
            ClockEntity& c = clocks_[n_clocks_++];
            c.id = d[3]; c.subtype = subtype; c.n_pins = d[4];
            c.source_id = (c.n_pins > 0 && len >= 7 + c.n_pins) ? d[5] : 0;
            c.controls = (len >= 7 + c.n_pins) ? d[5 + c.n_pins] : 0;
        }
        return;
    case uac::AC_CLOCK_MULTIPLIER:
        if (!v2 || len < 7 || n_clocks_ >= USB_AUDIO_HOST_MAX_ENTITIES) return;
        {
            ClockEntity& c = clocks_[n_clocks_++];
            c.id = d[3]; c.subtype = subtype; c.source_id = d[4]; c.controls = d[5];
        }
        return;
    default:
        return;
    }
}

void AudioFunction::parseStreaming(const uint8_t* d, Format& pending)
{
    const uint8_t len = d[0];
    const uint8_t type = d[1];
    const bool v2 = version_ == UacVersion::V2;
    if (type == uac::DT_CS_INTERFACE) {
        const uint8_t subtype = d[2];
        if (subtype == uac::AS_GENERAL) {
            if (v2 && len >= 16) {
                pending.terminal_link = d[3];
                pending.pcm = d[5] == uac::FORMAT_TYPE_I && (d[6] & uac::UAC2_FORMAT_PCM) != 0;
                pending.channels = d[10];
            } else if (!v2 && len >= 7) {
                pending.terminal_link = d[3];
                pending.pcm = le16(d + 5) == uac::UAC1_FORMAT_PCM;
            }
        } else if (subtype == uac::AS_FORMAT_TYPE && d[3] == uac::FORMAT_TYPE_I) {
            if (v2 && len >= 6) {
                pending.subslot = d[4];
                pending.bits = d[5];
            } else if (!v2 && len >= 8) {
                pending.channels = d[4];
                pending.subslot = d[5];
                pending.bits = d[6];
                const uint8_t n = d[7];
                if (n == 0 && len >= 14) {
                    pending.rate_min = le24(d + 8);
                    pending.rate_max = le24(d + 11);
                } else {
                    for (uint8_t i = 0; i < n && 8 + 3 * i + 3 <= len; i++) {
                        if (pending.n_rates < USB_AUDIO_HOST_MAX_RATES) {
                            pending.rates[pending.n_rates++] = le24(d + 8 + 3 * i);
                        }
                    }
                }
            }
        }
    } else if (type == uac::DT_ENDPOINT && len >= 7) {
        if ((d[3] & uac::EP_TRANSFER_MASK) != uac::EP_TRANSFER_ISOCHRONOUS) return;
        Endpoint ep;
        ep.address = d[2];
        ep.attributes = d[3];
        const uint16_t size = le16(d + 4);
        ep.max_packet = size & 0x07FF;
        ep.mult = static_cast<uint8_t>(((size >> 11) & 0x03) + 1);
        ep.interval = d[6] ? d[6] : 1;
        if (ep.isFeedback()) {
            pending.feedback = ep;
        } else if (!pending.data.valid()) {
            pending.data = ep;
        }
    } else if (type == uac::DT_CS_ENDPOINT && len >= 4 && d[2] == uac::EP_GENERAL) {
        if (!v2) pending.rate_control = (d[3] & uac::UAC1_EP_SAMPLING_FREQ_CONTROL) != 0;
    }
}

// Keeps a pending alternate setting once it is complete, then resets it for
// the next one. Alt 0 and any setting without a data endpoint are idle
// settings and are dropped.
bool AudioFunction::commit(Format& pending)
{
    const bool keep = pending.data.valid() && pending.channels > 0 && pending.subslot > 0 &&
                      n_formats_ < USB_AUDIO_HOST_MAX_FORMATS;
    if (keep) formats_[n_formats_++] = pending;
    pending = Format();
    return keep;
}

const Format* AudioFunction::choose(bool input, uint8_t channels, uint32_t rate, uint16_t packet_limit) const
{
    const Format* best = nullptr;
    uint32_t best_score = 0xFFFFFFFF;
    for (uint8_t i = 0; i < n_formats_; i++) {
        const Format& f = formats_[i];
        if (f.input() != input || !f.pcm) continue;
        if (f.bits < 16 || f.subslot < 2 || f.subslot > 4) continue;
        if (f.channels > USB_AUDIO_HOST_MAX_CHANNELS) continue;
        if (f.packetBytes() > packet_limit) continue;
        if (!f.supportsRate(rate)) continue;
        // Lower is better: channel distance first, with too few channels
        // costing more than too many; then fewer bits; then wider subslots.
        uint32_t score = (f.channels >= channels)
                             ? static_cast<uint32_t>(f.channels - channels) * 16u
                             : 1024u + static_cast<uint32_t>(channels - f.channels) * 16u;
        score += static_cast<uint32_t>(32 - (f.bits > 32 ? 32 : f.bits)) >> 1;
        score += f.subslot;
        if (score < best_score) {
            best_score = score;
            best = &f;
        }
    }
    return best;
}

const Format* AudioFunction::implicitFeedbackSource(const Format& sink) const
{
    if (sink.input() || sink.data.sync() != SyncType::Async || sink.feedback.valid()) return nullptr;
    const Format* fallback = nullptr;
    for (uint8_t i = 0; i < n_formats_; i++) {
        const Format& f = formats_[i];
        if (!f.input()) continue;
        if (f.data.isImplicitFeedback()) return &f;
        if (f.data.sync() == SyncType::Async && !fallback) fallback = &f;
    }
    return fallback;
}

const Terminal* AudioFunction::findTerminal(uint8_t id) const
{
    for (uint8_t i = 0; i < n_terminals_; i++) {
        if (terminals_[i].id == id) return &terminals_[i];
    }
    return nullptr;
}

const ClockEntity* AudioFunction::findClock(uint8_t id) const
{
    for (uint8_t i = 0; i < n_clocks_; i++) {
        if (clocks_[i].id == id) return &clocks_[i];
    }
    return nullptr;
}

bool AudioFunction::clockFor(const Format& f, ClockPath& path) const
{
    path = ClockPath();
    if (version_ != UacVersion::V2) return true; // UAC1 clocks the endpoint itself
    const Terminal* t = findTerminal(f.terminal_link);
    if (!t) return false;
    uint8_t id = t->clock_id;
    for (int hops = 0; hops < 4 && id != 0; hops++) {
        const ClockEntity* c = findClock(id);
        if (!c) return false;
        if (c->subtype == uac::AC_CLOCK_SOURCE) {
            path.source = c->id;
            return true;
        }
        if (c->subtype == uac::AC_CLOCK_SELECTOR) {
            path.selector = c->id;
            path.selector_pin = 1;
        }
        id = c->source_id;
    }
    return false;
}

} // namespace usb_audio_host
