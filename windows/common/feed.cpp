#include "feed.h"

#include <algorithm>
#include <atomic>

#include "vcam/draw.h"

namespace vcam::win {

namespace {

constexpr int64_t kAttachRetry = shm::kTicksPerSecond / 4;

// A token unique to this feed among all consumers of the camera.
uint32_t NewOwnerToken()
{
    static std::atomic<uint32_t> counter{0};
    uint64_t x = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ static_cast<uint64_t>(NowTicks()) ^
                 (static_cast<uint64_t>(counter.fetch_add(1)) << 48);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    const auto token = static_cast<uint32_t>(x);
    return token ? token : 1;
}

}  // namespace

FrameFeed::FrameFeed(int camera, bool allow_local) : camera_(camera), allow_local_(allow_local), config_(LoadConfig())
{
    request_.owner = NewOwnerToken();
}

void FrameFeed::Configure(PixelFormat format, int width, int height, uint32_t fps_num, uint32_t fps_den)
{
    format_ = format;
    width_ = width;
    height_ = height;
    const uint32_t owner = request_.owner;
    request_ = shm::FrameRequest{};
    request_.format = static_cast<uint32_t>(format);
    request_.width = static_cast<uint32_t>(width);
    request_.height = static_cast<uint32_t>(height);
    request_.fps_num = fps_num;
    request_.fps_den = fps_den;
    request_.colorspace = static_cast<uint32_t>(ColorSpace::Bt601Limited);
    request_.owner = owner;
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
        RenderPlaceholder(canvas, "No video source", "Open VCam and pick a source");
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
        if (shm_.Open(camera_, true, capacity, allow_local_)) {
            Log("camera %d: attached to shared memory (%s%s, %llu bytes per slot)", camera_ + 1,
                shm_.created() ? "created" : "opened", shm_.local() ? " in this session" : "",
                static_cast<unsigned long long>(shm_.ring().capacity()));
            request_published_ = false;
        }
    }
    if (!shm_.is_open()) {
        return;
    }
    shm::Header* h = shm_.ring().header();
    // The request belongs to whoever wrote it, for as long as it streams.
    shm::FrameRequest current;
    const bool have = shm_.ring().ReadRequest(&current);
    const bool mine = have && current.owner == request_.owner;
    const bool other_streaming = have && !mine && current.owner != 0 && ConsumerStreaming(h, now);
    if (other_streaming) {
        if (owner_) {
            Log("camera %d: another application now sets the format", camera_ + 1);
        }
        owner_ = false;
        request_published_ = false;
    } else {
        if (!owner_) {
            Log("camera %d: asking the producer for %ux%u %s", camera_ + 1, request_.width, request_.height,
                FormatName(static_cast<PixelFormat>(request_.format)));
        }
        owner_ = true;
        if (!request_published_ || !mine) {
            shm_.ring().WriteRequest(request_);
            if (shm_.request_event()) {
                SetEvent(shm_.request_event());
            }
            request_published_ = true;
        }
        AtomicStore(h->consumer_pid, static_cast<uint32_t>(GetCurrentProcessId()));
        AtomicStore(h->consumer_flags, shm::kConsumerStreaming);
        AtomicStore(h->consumer_heartbeat, now);
    }
    LogProducerState(now);
}

void FrameFeed::Stop()
{
    if (!shm_.is_open() || !owner_) {
        return;
    }
    owner_ = false;
    request_published_ = false;
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

uint64_t FrameFeed::Render(const ImageSpan& dst, int64_t now, bool flip)
{
    if (producer_alive(now)) {
        uint64_t frame = 0;
        const Ring::ReadStatus status = shm_.ring().ReadLatest(
            [&](const ImageView& view, const shm::SlotHeader& slot) {
                ConvertOptions options;
                options.input_colorspace = static_cast<ColorSpace>(slot.colorspace);
                options.output_colorspace = ColorSpace::Bt601Limited;
                options.flip = flip;
                // Rendered for another consumer's size: rescale only the
                // picture, not the padding around it.
                const bool other_size = view.width != dst.width || view.height != dst.height;
                converter_.Convert(other_size ? SlotContent(slot, view) : view, dst, options);
            },
            &frame);
        if (status == Ring::ReadStatus::Ok) {
            return frame;
        }
    }
    ConvertOptions options;
    options.flip = flip;
    converter_.Convert(MakeView(format_, width_, height_, placeholder_.data()), dst, options);
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
