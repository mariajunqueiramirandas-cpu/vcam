#include "source.h"

#include "vcam/draw.h"

namespace vcam::win {

namespace {

constexpr int64_t kAttachRetry = shm::kTicksPerSecond / 4;

}  // namespace

FrameFeed::FrameFeed(int camera) : camera_(camera), config_(LoadConfig())
{
}

void FrameFeed::Configure(PixelFormat format, int width, int height, uint32_t fps_num, uint32_t fps_den)
{
    format_ = format;
    width_ = width;
    height_ = height;
    request_ = shm::FrameRequest{};
    request_.format = static_cast<uint32_t>(format);
    request_.width = static_cast<uint32_t>(width);
    request_.height = static_cast<uint32_t>(height);
    request_.fps_num = fps_num;
    request_.fps_den = fps_den;
    request_.colorspace = static_cast<uint32_t>(ColorSpace::Bt601Limited);
    request_published_ = false;

    // Pre-render the placeholder in the output format; delivering it is then a copy.
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
    const ImageSpan canvas = MakeSpan(PixelFormat::BGRA, width, height, bgra.data());
    std::string image;
    uint32_t iw = 0, ih = 0;
    if (!config_.placeholder.empty() && LoadPlaceholderImage(config_.placeholder, &image, &iw, &ih)) {
        ConvertOptions fit;
        converter_.Convert(MakeView(PixelFormat::BGRA, static_cast<int>(iw), static_cast<int>(ih),
                                    reinterpret_cast<const uint8_t*>(image.data())),
                           canvas, fit);
    } else {
        RenderPlaceholder(canvas, "No video source", "Start a producer, e.g.  vcam bars");
    }
    PlaneLayout layout;
    DefaultLayout(format, static_cast<uint32_t>(width), static_cast<uint32_t>(height), &layout);
    placeholder_.assign(static_cast<size_t>(layout.total_bytes), 0);
    converter_.Convert(canvas.view(), MakeSpan(format, width, height, placeholder_.data()), ConvertOptions{});
}

void FrameFeed::Tick(int64_t now)
{
    if (!shm_.is_open() && now >= next_attach_) {
        next_attach_ = now + kAttachRetry;
        const uint64_t capacity = SlotCapacityFor(std::max<uint32_t>(config_.max_width, 1920),
                                                  std::max<uint32_t>(config_.max_height, 1080));
        if (shm_.Open(camera_, true, capacity)) {
            Log("camera %d: attached to shared memory (%s, %llu bytes per slot)", camera_ + 1,
                shm_.created() ? "created" : "opened", static_cast<unsigned long long>(shm_.ring().capacity()));
            request_published_ = false;
        }
    }
    if (!shm_.is_open()) {
        return;
    }
    shm::Header* h = shm_.ring().header();
    if (!request_published_) {
        shm_.ring().WriteRequest(request_);
        if (shm_.request_event()) {
            SetEvent(shm_.request_event());
        }
        request_published_ = true;
    }
    AtomicStore(h->consumer_pid, static_cast<uint32_t>(GetCurrentProcessId()));
    AtomicStore(h->consumer_flags, shm::kConsumerStreaming);
    AtomicStore(h->consumer_heartbeat, now);
    LogProducerState(now);
}

void FrameFeed::Stop()
{
    if (!shm_.is_open()) {
        return;
    }
    shm::Header* h = shm_.ring().header();
    AtomicStore(h->consumer_flags, 0u);
    AtomicStore(h->consumer_pid, 0u);
    if (shm_.request_event()) {
        SetEvent(shm_.request_event());
    }
}

uint64_t FrameFeed::latest() const
{
    return shm_.is_open() ? shm_.ring().latest_frame() : 0;
}

bool FrameFeed::producer_alive(int64_t now) const
{
    return shm_.is_open() && ProducerAlive(shm_.ring().header(), now);
}

uint64_t FrameFeed::Render(const ImageSpan& dst, int64_t now)
{
    if (producer_alive(now)) {
        uint64_t frame = 0;
        const Ring::ReadStatus status = shm_.ring().ReadLatest(
            [&](const ImageView& view, const shm::SlotHeader& slot) {
                ConvertOptions options;
                options.input_colorspace = static_cast<ColorSpace>(slot.colorspace);
                options.output_colorspace = ColorSpace::Bt601Limited;
                converter_.Convert(view, dst, options);
            },
            &frame);
        if (status == Ring::ReadStatus::Ok) {
            return frame;
        }
    }
    converter_.Convert(MakeView(format_, width_, height_, placeholder_.data()), dst, ConvertOptions{});
    return 0;
}

void FrameFeed::CountDelivery(bool fresh)
{
    if (!shm_.is_open()) {
        return;
    }
    shm::Header* h = shm_.ring().header();
    std::atomic_ref<uint64_t>(fresh ? h->frames_delivered : h->frames_repeated)
        .fetch_add(1, std::memory_order_relaxed);
}

void FrameFeed::LogProducerState(int64_t now)
{
    const bool alive = producer_alive(now);
    if (alive != producer_was_alive_) {
        producer_was_alive_ = alive;
        Log("camera %d: producer %s", camera_ + 1, alive ? "connected" : "gone, showing placeholder");
    }
}

}  // namespace vcam::win
