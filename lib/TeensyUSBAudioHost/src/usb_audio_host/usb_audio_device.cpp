#if defined(__MK66FX1M0__) || defined(__IMXRT1052__) || defined(__IMXRT1062__)

#include "usb_audio_host/usb_audio_device.h"
#include "usb_audio_host/uac.h"

using namespace usb_audio_host;

// The largest packet an alternate setting may use and still fit the buffers.
static const uint16_t PACKET_LIMIT = USB_AUDIO_HOST_PACKET_BYTES & ~31u;

static const uint32_t CONTROL_TIMEOUT_US = 500000;
static const uint8_t CONTROL_RETRIES = 3;

static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Wire samples are little-endian, left-justified in their subslot (UAC1
// 2.2.1.1); the rings hold them left-justified in 32 bits.
static inline int32_t decode(const uint8_t* p, uint8_t subslot)
{
    switch (subslot) {
    case 2:
        return static_cast<int32_t>(static_cast<int16_t>(p[0] | (p[1] << 8))) * 65536;
    case 3:
        return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                    (static_cast<uint32_t>(p[2]) << 24));
    default:
        return static_cast<int32_t>(le32(p));
    }
}

static inline void encode(int32_t v, uint8_t* p, uint8_t subslot)
{
    const uint32_t u = static_cast<uint32_t>(v);
    switch (subslot) {
    case 2:
        p[0] = static_cast<uint8_t>(u >> 16);
        p[1] = static_cast<uint8_t>(u >> 24);
        break;
    case 3:
        p[0] = static_cast<uint8_t>(u >> 8);
        p[1] = static_cast<uint8_t>(u >> 16);
        p[2] = static_cast<uint8_t>(u >> 24);
        break;
    default:
        p[0] = static_cast<uint8_t>(u);
        p[1] = static_cast<uint8_t>(u >> 8);
        p[2] = static_cast<uint8_t>(u >> 16);
        p[3] = static_cast<uint8_t>(u >> 24);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

USBAudioDevice::USBAudioDevice(USBHost& host)
    : USBDriver(), function_(), clock_(), timer_(this), transfers_(), in_stream_(), out_stream_(), fb_stream_(), in_desc_(), out_desc_(), fb_desc_(),
      in_buf_(), out_buf_(), fb_buf_(), pacer_(), in_ring_(), out_ring_()
{
    (void)host;
    init();
}

void USBAudioDevice::init()
{
    // Three control transfers per request; a handful so the pool shared
    // with every other driver is not what decides whether we configure.
    contribute_Transfers(transfers_, sizeof(transfers_) / sizeof(transfers_[0]));
    driver_ready_for_device(this);
}

bool USBAudioDevice::claim(Device_t* dev, int type, const uint8_t* descriptors, uint32_t len)
{
    if (type != 1) return false; // interfaces only; the whole device is never ours
    if (!function_.parse(descriptors, len)) return false;
    high_speed_ = dev->speed == 2;
    rate_ = wanted_rate_;
    in_format_ = function_.choose(true, wanted_in_, rate_, PACKET_LIMIT);
    out_format_ = function_.choose(false, wanted_out_, rate_, PACKET_LIMIT);
    if (!in_format_ && !out_format_ && function_.version() == UacVersion::V1) {
        // No UAC1 format lists the wanted rate: take the first rate of the
        // best format instead. (A UAC2 clock is asked for its range later.)
        const Format* any = function_.choose(true, wanted_in_, 0, PACKET_LIMIT);
        if (!any) any = function_.choose(false, wanted_out_, 0, PACKET_LIMIT);
        if (any) {
            rate_ = any->n_rates ? any->rates[0] : any->rate_min;
            in_format_ = function_.choose(true, wanted_in_, rate_, PACKET_LIMIT);
            out_format_ = function_.choose(false, wanted_out_, rate_, PACKET_LIMIT);
        }
    }
    if (!in_format_ && !out_format_) {
        function_.clear();
        return false;
    }
    device = dev;
    failure_ = "";
    clock_ = ClockPath();
    clock_programmable_ = true;
    if (function_.version() == UacVersion::V2) {
        const Format* f = in_format_ ? in_format_ : out_format_;
        if (!function_.clockFor(*f, clock_) || clock_.source == 0) {
            fail("no clock source behind the streaming terminal");
            return true;
        }
        const ClockEntity* source = function_.findClock(clock_.source);
        clock_programmable_ = source && source->programmable();
    }
    IsoEngine::install();
    IsoEngine::add(&in_stream_);
    IsoEngine::add(&out_stream_);
    IsoEngine::add(&fb_stream_);
    state_ = State::Configuring;
    step_ = STEP_NONE;
    retries_ = 0;
    advance();
    return true;
}

void USBAudioDevice::disconnect()
{
    timer_.stop();
    stopStreams();
    IsoEngine::remove(&in_stream_);
    IsoEngine::remove(&out_stream_);
    IsoEngine::remove(&fb_stream_);
    state_ = State::Idle;
    step_ = STEP_NONE;
    in_format_ = out_format_ = nullptr;
    rate_ = 0;
    implicit_ = false;
    fb_active_ = false;
    block_pending_ = false;
    in_frame_bytes_ = out_frame_bytes_ = 0;
    function_.clear();
}

void USBAudioDevice::fail(const char* why)
{
    timer_.stop();
    stopStreams();
    failure_ = why;
    state_ = State::Failed;
}

void USBAudioDevice::Task()
{
    if (state_ == State::Streaming) IsoEngine::serviceMasked();
}

/* ------------------------------------------------------------------------- */
/* Control plane                                                             */
/* ------------------------------------------------------------------------- */

// Walks the steps in order; a step that does not apply to this device is
// skipped. Each issued request ends in control() or, failing that, in
// timer_event().
void USBAudioDevice::advance()
{
    while (step_ < STEP_START) {
        step_ = static_cast<Step>(step_ + 1);
        if (step_ == STEP_START) break;
        if (issue(step_)) {
            timer_.start(CONTROL_TIMEOUT_US);
            return;
        }
    }
    step_ = STEP_DONE;
    startStreams();
}

bool USBAudioDevice::send(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length)
{
    mk_setup(setup_, request_type, request, value, index, length);
    return queue_Control_Transfer(device, &setup_, length ? control_buf_ : nullptr, this);
}

bool USBAudioDevice::issue(Step step)
{
    const bool v2 = function_.version() == UacVersion::V2;
    const uint16_t ac = function_.controlInterface();
    switch (step) {
    case STEP_GET_RANGE:
        if (!v2) return false;
        return send(uac::RT_GET_CLASS_INTERFACE, uac::UAC2_RANGE, uac::UAC2_CS_SAM_FREQ_CONTROL << 8,
                    static_cast<uint16_t>((clock_.source << 8) | ac), sizeof(control_buf_));
    case STEP_SET_SELECTOR:
        if (!v2 || clock_.selector == 0) return false;
        control_buf_[0] = clock_.selector_pin;
        return send(uac::RT_SET_CLASS_INTERFACE, uac::UAC2_CUR, uac::UAC2_CX_CLOCK_SELECTOR_CONTROL << 8,
                    static_cast<uint16_t>((clock_.selector << 8) | ac), 1);
    case STEP_SET_RATE:
        if (!v2 || !clock_programmable_) return false;
        control_buf_[0] = static_cast<uint8_t>(rate_);
        control_buf_[1] = static_cast<uint8_t>(rate_ >> 8);
        control_buf_[2] = static_cast<uint8_t>(rate_ >> 16);
        control_buf_[3] = static_cast<uint8_t>(rate_ >> 24);
        return send(uac::RT_SET_CLASS_INTERFACE, uac::UAC2_CUR, uac::UAC2_CS_SAM_FREQ_CONTROL << 8,
                    static_cast<uint16_t>((clock_.source << 8) | ac), 4);
    case STEP_GET_RATE:
        if (!v2 || clock_programmable_) return false;
        return send(uac::RT_GET_CLASS_INTERFACE, uac::UAC2_CUR, uac::UAC2_CS_SAM_FREQ_CONTROL << 8,
                    static_cast<uint16_t>((clock_.source << 8) | ac), 4);
    case STEP_SET_IFACE_IN:
        if (!in_format_) return false;
        return send(uac::RT_SET_INTERFACE, uac::REQ_SET_INTERFACE, in_format_->alt, in_format_->interface, 0);
    case STEP_SET_RATE_IN:
        if (v2 || !in_format_ || !in_format_->rate_control) return false;
        control_buf_[0] = static_cast<uint8_t>(rate_);
        control_buf_[1] = static_cast<uint8_t>(rate_ >> 8);
        control_buf_[2] = static_cast<uint8_t>(rate_ >> 16);
        return send(uac::RT_SET_CLASS_ENDPOINT, uac::UAC1_SET_CUR, uac::UAC1_SAMPLING_FREQ_CONTROL << 8,
                    in_format_->data.address, 3);
    case STEP_SET_IFACE_OUT:
        if (!out_format_) return false;
        return send(uac::RT_SET_INTERFACE, uac::REQ_SET_INTERFACE, out_format_->alt, out_format_->interface, 0);
    case STEP_SET_RATE_OUT:
        if (v2 || !out_format_ || !out_format_->rate_control) return false;
        control_buf_[0] = static_cast<uint8_t>(rate_);
        control_buf_[1] = static_cast<uint8_t>(rate_ >> 8);
        control_buf_[2] = static_cast<uint8_t>(rate_ >> 16);
        return send(uac::RT_SET_CLASS_ENDPOINT, uac::UAC1_SET_CUR, uac::UAC1_SAMPLING_FREQ_CONTROL << 8,
                    out_format_->data.address, 3);
    default:
        return false;
    }
}

void USBAudioDevice::control(const Transfer_t* transfer)
{
    if (transfer->driver != this || state_ != State::Configuring) return;
    timer_.stop();
    retries_ = 0;
    switch (step_) {
    case STEP_GET_RANGE:
        chooseRateFromRange();
        break;
    case STEP_GET_RATE:
        rate_ = le32(control_buf_);
        break;
    default:
        break;
    }
    advance();
}

void USBAudioDevice::timer_event(USBDriverTimer* timer)
{
    if (timer != &timer_ || state_ != State::Configuring) return;
    if (++retries_ > CONTROL_RETRIES) {
        fail("the device did not answer a control request");
        return;
    }
    if (issue(step_)) {
        timer_.start(CONTROL_TIMEOUT_US);
    } else {
        advance();
    }
}

// A UAC2 RANGE reply for the sampling frequency: wNumSubRanges, then
// (dMIN, dMAX, dRES) triples. The wanted rate stands if some subrange holds
// it; otherwise the nearest rate any subrange offers.
void USBAudioDevice::chooseRateFromRange()
{
    const uint16_t n = le16(control_buf_);
    const uint16_t fit = static_cast<uint16_t>((sizeof(control_buf_) - 2) / 12);
    const uint16_t count = n < fit ? n : fit;
    if (count == 0) return;
    uint32_t best = 0;
    uint32_t best_distance = 0xFFFFFFFF;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* r = control_buf_ + 2 + 12 * i;
        const uint32_t lo = le32(r), hi = le32(r + 4), res = le32(r + 8);
        if (lo == 0) continue;
        if (rate_ >= lo && rate_ <= hi && (res == 0 ? rate_ == lo : ((rate_ - lo) % res) == 0)) return;
        uint32_t candidate = lo;
        if (res && rate_ > lo) {
            candidate = lo + ((rate_ - lo + res / 2) / res) * res;
            if (candidate > hi) candidate = hi;
        } else if (rate_ > hi) {
            candidate = hi;
        }
        const uint32_t distance = candidate > rate_ ? candidate - rate_ : rate_ - candidate;
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    if (best) rate_ = best;
}

/* ------------------------------------------------------------------------- */
/* Streams                                                                   */
/* ------------------------------------------------------------------------- */

void USBAudioDevice::startStreams()
{
    IsoStream::Config c;
    c.device_address = device->address;
    c.hub_address = device->hub_address;
    c.hub_port = device->hub_port;
    c.high_speed = high_speed_;

    implicit_ = false;
    fb_active_ = false;
    in_frame_bytes_ = out_frame_bytes_ = 0;

    if (in_format_) {
        c.endpoint = in_format_->data.number();
        c.in = true;
        c.max_packet = in_format_->data.max_packet;
        c.mult = in_format_->data.mult;
        c.interval = in_format_->data.interval;
        in_stream_.attach(in_desc_, USB_AUDIO_HOST_QUEUE_FRAMES, in_buf_, sizeof(in_buf_));
        if (!in_stream_.configure(c)) {
            fail("the input stream does not fit the engine's buffers");
            return;
        }
        in_stream_.onReceive(inReceiveThunk, this);
        in_frame_bytes_ = static_cast<uint8_t>(in_format_->channels * in_format_->subslot);
        in_ring_.begin(in_format_->channels);
    }
    if (out_format_) {
        c.endpoint = out_format_->data.number();
        c.in = false;
        c.max_packet = out_format_->data.max_packet;
        c.mult = out_format_->data.mult;
        c.interval = out_format_->data.interval;
        out_stream_.attach(out_desc_, USB_AUDIO_HOST_QUEUE_FRAMES, out_buf_, sizeof(out_buf_));
        if (!out_stream_.configure(c)) {
            fail("the output stream does not fit the engine's buffers");
            return;
        }
        out_stream_.onProduce(outProduceThunk, this);
        out_frame_bytes_ = static_cast<uint8_t>(out_format_->channels * out_format_->subslot);
        out_ring_.begin(out_format_->channels);
        pacer_.begin(rate_, out_stream_.packetsPerSecond());

        const Format* source = function_.implicitFeedbackSource(*out_format_);
        implicit_ = source && in_format_ && source->data.address == in_format_->data.address &&
                    in_stream_.packetsPerSecond() == out_stream_.packetsPerSecond();

        if (out_format_->feedback.valid()) {
            IsoStream::Config f = c;
            f.endpoint = out_format_->feedback.number();
            f.in = true;
            f.max_packet = out_format_->feedback.max_packet;
            f.mult = 1;
            f.interval = out_format_->feedback.interval;
            fb_stream_.attach(fb_desc_, USB_AUDIO_HOST_QUEUE_FRAMES, fb_buf_, sizeof(fb_buf_));
            if (fb_stream_.configure(f)) {
                fb_stream_.onReceive(fbReceiveThunk, this);
                fb_active_ = true;
            }
        }
    }
    feedback_packets_ = 0;
    block_pending_ = false;
    state_ = State::Streaming;
    if (in_format_) in_stream_.start();
    if (fb_active_) fb_stream_.start();
    if (out_format_) out_stream_.start();
}

void USBAudioDevice::stopStreams()
{
    in_stream_.stop();
    out_stream_.stop();
    fb_stream_.stop();
}

void USBAudioDevice::inReceiveThunk(void* ctx, const uint8_t* data, uint16_t len, bool ok)
{
    static_cast<USBAudioDevice*>(ctx)->inReceive(data, len, ok);
}

uint16_t USBAudioDevice::outProduceThunk(void* ctx, uint8_t* data, uint16_t capacity)
{
    return static_cast<USBAudioDevice*>(ctx)->outProduce(data, capacity);
}

void USBAudioDevice::fbReceiveThunk(void* ctx, const uint8_t* data, uint16_t len, bool ok)
{
    USBAudioDevice* self = static_cast<USBAudioDevice*>(ctx);
    if (ok && len >= 3) {
        self->pacer_.feedback(data, static_cast<uint8_t>(len));
        self->feedback_packets_++;
    }
}

void USBAudioDevice::inReceive(const uint8_t* data, uint16_t len, bool ok)
{
    if (in_frame_bytes_ == 0 || !in_format_) return;
    const uint16_t frames = static_cast<uint16_t>(len / in_frame_bytes_);
    if (ok && frames) {
        const uint8_t channels = in_format_->channels;
        const uint8_t subslot = in_format_->subslot;
        const uint8_t* p = data;
        for (uint16_t f = 0; f < frames; f++) {
            if (!in_ring_.beginFrame()) break;
            for (uint8_t ch = 0; ch < channels; ch++) {
                in_ring_.sample(ch, decode(p, subslot));
                p += subslot;
            }
            in_ring_.endFrame();
        }
        if (implicit_) pacer_.mirror(frames);
    }
    if (block_fn_ && !block_pending_ && in_ring_.available() >= block_frames_) {
        block_pending_ = true;
        block_fn_(block_ctx_);
    }
}

uint16_t USBAudioDevice::outProduce(uint8_t* data, uint16_t capacity)
{
    if (out_frame_bytes_ == 0 || !out_format_) return 0;
    uint16_t frames = pacer_.next();
    const uint16_t fit = static_cast<uint16_t>(capacity / out_frame_bytes_);
    if (frames > fit) frames = fit;
    const uint8_t channels = out_format_->channels;
    const uint8_t subslot = out_format_->subslot;
    int32_t frame[USB_AUDIO_HOST_MAX_CHANNELS];
    uint8_t* p = data;
    for (uint16_t f = 0; f < frames; f++) {
        out_ring_.read(frame, 1);
        for (uint8_t ch = 0; ch < channels; ch++) {
            encode(frame[ch], p, subslot);
            p += subslot;
        }
    }
    if (!in_format_ && block_fn_ && !block_pending_ && out_ring_.available() < 2u * block_frames_ &&
        out_ring_.space() >= block_frames_) {
        block_pending_ = true;
        block_fn_(block_ctx_);
    }
    return static_cast<uint16_t>(frames * out_frame_bytes_);
}

/* ------------------------------------------------------------------------- */
/* Sketch-facing                                                             */
/* ------------------------------------------------------------------------- */

void USBAudioDevice::onBlock(BlockFn fn, void* context, uint16_t frames)
{
    block_fn_ = nullptr;
    block_ctx_ = context;
    block_frames_ = frames;
    block_pending_ = false;
    block_fn_ = fn;
}

USBAudioDevice::Stats USBAudioDevice::stats() const
{
    Stats s;
    s.in_packets = in_stream_.packets();
    s.in_errors = in_stream_.errors();
    s.in_missed = in_stream_.missed();
    s.in_late = in_stream_.late();
    s.in_overruns = in_ring_.overruns();
    s.out_packets = out_stream_.packets();
    s.out_errors = out_stream_.errors();
    s.out_missed = out_stream_.missed();
    s.out_late = out_stream_.late();
    s.out_underruns = out_ring_.underruns();
    s.feedback_packets = feedback_packets_;
    s.feedback_ignored = pacer_.feedbackIgnored();
    s.rate_q16 = pacer_.rateQ16();
    return s;
}

static void printFormat(Print& out, const char* label, const Format* f)
{
    out.print(label);
    if (!f) {
        out.println("none");
        return;
    }
    out.print("interface ");
    out.print(f->interface);
    out.print(" alt ");
    out.print(f->alt);
    out.print(", ");
    out.print(f->channels);
    out.print(" ch, ");
    out.print(f->bits);
    out.print(" bit in ");
    out.print(f->subslot);
    out.print(" bytes, endpoint 0x");
    out.print(f->data.address, HEX);
    out.print(", ");
    out.print(f->data.max_packet);
    out.print(" bytes x");
    out.print(f->data.mult);
    out.print(" every 2^");
    out.print(f->data.interval - 1);
    static const char* const sync[] = {"no sync", "asynchronous", "adaptive", "synchronous"};
    out.print(", ");
    out.print(sync[static_cast<uint8_t>(f->data.sync())]);
    if (f->feedback.valid()) {
        out.print(", feedback endpoint 0x");
        out.print(f->feedback.address, HEX);
    }
    if (f->data.isImplicitFeedback()) out.print(", implicit feedback source");
    out.println();
}

void USBAudioDevice::printStatus(Print& out) const
{
    static const char* const states[] = {"idle", "configuring", "streaming", "failed"};
    out.print("USB audio: ");
    out.print(states[static_cast<uint8_t>(state_)]);
    if (state_ == State::Failed) {
        out.print(" (");
        out.print(failure_);
        out.print(")");
    }
    if (state_ == State::Idle) {
        out.println();
        return;
    }
    out.print(", UAC");
    out.print(function_.version() == UacVersion::V2 ? 2 : 1);
    out.print(high_speed_ ? ", high speed" : ", full speed");
    out.print(", ");
    out.print(rate_);
    out.println(" Hz");
    printFormat(out, "  input:  ", in_format_);
    printFormat(out, "  output: ", out_format_);
    if (out_format_) {
        out.print("  output pacing: ");
        out.print(fb_active_ ? "explicit feedback" : (implicit_ ? "implicit feedback" : "nominal rate"));
        out.print(", ");
        out.print(static_cast<float>(pacer_.rateQ16()) / 65536.0f, 4);
        out.println(" frames per packet");
    }
    const Stats s = stats();
    out.print("  in: ");
    out.print(s.in_packets);
    out.print(" packets, ");
    out.print(s.in_errors);
    out.print(" errors, ");
    out.print(s.in_missed);
    out.print(" missed, ");
    out.print(s.in_late);
    out.print(" late, ");
    out.print(s.in_overruns);
    out.println(" overrun frames");
    out.print("  out: ");
    out.print(s.out_packets);
    out.print(" packets, ");
    out.print(s.out_errors);
    out.print(" errors, ");
    out.print(s.out_missed);
    out.print(" missed, ");
    out.print(s.out_late);
    out.print(" late, ");
    out.print(s.out_underruns);
    out.println(" underrun frames");
    if (fb_active_) {
        out.print("  feedback: ");
        out.print(s.feedback_packets);
        out.print(" packets, ");
        out.print(s.feedback_ignored);
        out.println(" ignored");
    }
}

#endif
