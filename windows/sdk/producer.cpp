// The producer side of the vcam SDK: converts frames into the camera's shared
// memory, in exactly the format the camera currently delivers.
#include "vcam.h"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "common.h"
#include "vcam/convert.h"

using namespace vcam;
using namespace vcam::win;

struct vcam_producer {
    int index = 0;  // 0-based camera
    ConvertOptions options;
    uint64_t capacity = 0;
    bool allow_local = false;  // DirectShow backend: the ring may live in this session

    std::mutex lock;  // everything below
    SharedFrames shm;
    Converter converter;
    // A frame sent while no shared memory existed yet (non-elevated producer, no
    // application using the camera); published as soon as it appears.
    std::vector<uint8_t> pending;
    ImageView pending_view;
    ColorSpace pending_colorspace = ColorSpace::Bt601Limited;
    int64_t pending_timestamp = 0;
    bool has_pending = false;
    // The target the newest slot was rendered for.
    shm::FrameRequest rendered{};
    bool has_rendered = false;

    std::thread worker;
    HANDLE stop_event = nullptr;
    std::atomic<uint64_t> frames_sent{0};
};

namespace {

constexpr int64_t kAttachRetry = shm::kTicksPerSecond / 4;
constexpr DWORD kWorkerTick = 200;  // ms; heartbeat period

bool RequestUsable(const shm::FrameRequest& r)
{
    return IsValidFormat(r.format) && IsValidColorSpace(r.colorspace) && r.width >= 16 && r.height >= 16 &&
           r.width <= kMaxDimension && r.height <= kMaxDimension;
}

bool SameTarget(const shm::FrameRequest& a, const shm::FrameRequest& b)
{
    return a.format == b.format && a.width == b.width && a.height == b.height && a.colorspace == b.colorspace;
}

bool Fits(PixelFormat format, uint32_t width, uint32_t height, uint64_t capacity)
{
    shm::SlotHeader probe{};
    return LayoutSlot(format, width, height, &probe) && probe.data_bytes <= capacity;
}

// The format to publish: what the camera asked for, or, before any application
// asked, the input's own format and size (shrunk if the slot is too small).
shm::FrameRequest ChooseTarget(vcam_producer* p, const ImageView& src)
{
    shm::FrameRequest r;
    if (p->shm.ring().ReadRequest(&r) && RequestUsable(r) &&
        Fits(static_cast<PixelFormat>(r.format), r.width, r.height, p->shm.ring().capacity())) {
        return r;
    }
    r = shm::FrameRequest{};
    r.format = static_cast<uint32_t>(src.format);
    uint32_t w = static_cast<uint32_t>(src.width), h = static_cast<uint32_t>(src.height);
    while (!Fits(src.format, w, h, p->shm.ring().capacity()) && w > 16 && h > 16) {
        w = std::max<uint32_t>(16, w * 3 / 4);
        h = std::max<uint32_t>(16, h * 3 / 4);
    }
    r.width = w;
    r.height = h;
    r.colorspace = static_cast<uint32_t>(ColorSpace::Bt601Limited);
    return r;
}

// Converts src into the next slot and publishes it. Caller holds p->lock.
bool WriteFrame(vcam_producer* p, const ImageView& src, ColorSpace src_colorspace, int64_t timestamp,
                const ConvertOptions& base)
{
    Ring& ring = p->shm.ring();
    const shm::FrameRequest target = ChooseTarget(p, src);
    const auto format = static_cast<PixelFormat>(target.format);
    shm::SlotHeader info{};
    if (!LayoutSlot(format, target.width, target.height, &info)) {
        return false;
    }
    info.colorspace = target.colorspace;
    info.timestamp = timestamp ? timestamp : NowTicks();
    const Placement pl = ComputePlacement(src.width, src.height, static_cast<int>(target.width),
                                          static_cast<int>(target.height), base.scale, IsYuv(format));
    if (pl.dst_x != 0 || pl.dst_y != 0 || pl.dst_w != static_cast<int>(target.width) ||
        pl.dst_h != static_cast<int>(target.height)) {
        info.content[0] = static_cast<uint32_t>(pl.dst_x);
        info.content[1] = static_cast<uint32_t>(pl.dst_y);
        info.content[2] = static_cast<uint32_t>(pl.dst_w);
        info.content[3] = static_cast<uint32_t>(pl.dst_h);
    }

    const Ring::WriteTarget slot = ring.BeginWrite();
    if (!slot.data || info.data_bytes > slot.capacity) {
        return false;
    }
    ImageSpan dst;
    dst.format = format;
    dst.width = static_cast<int>(target.width);
    dst.height = static_cast<int>(target.height);
    for (int i = 0; i < PlaneCount(format); i++) {
        dst.plane[i] = slot.data + info.offset[i];
        dst.pitch[i] = static_cast<int>(info.pitch[i]);
    }
    ConvertOptions options = base;
    options.input_colorspace = src_colorspace;
    options.output_colorspace = static_cast<ColorSpace>(target.colorspace);
    if (!p->converter.Convert(src, dst, options)) {
        return false;
    }
    ring.CommitWrite(slot, info);
    AtomicStore(ring.header()->producer_heartbeat, NowTicks());
    if (p->shm.frame_event()) {
        SetEvent(p->shm.frame_event());
    }
    p->rendered = target;
    p->has_rendered = true;
    p->frames_sent++;
    return true;
}

// The camera now wants a different format: convert the newest frame again so
// a producer that sent a single image keeps showing it correctly. Only the
// picture area is reused, so padding is not padded again; mirroring was
// already applied the first time.
void Rerender(vcam_producer* p)
{
    shm::FrameRequest r;
    Ring& ring = p->shm.ring();
    if (!p->has_rendered || !ring.ReadRequest(&r) || !RequestUsable(r) || SameTarget(r, p->rendered) ||
        !Fits(static_cast<PixelFormat>(r.format), r.width, r.height, ring.capacity())) {
        return;
    }
    ConvertOptions options = p->options;
    options.mirror = false;
    options.flip = false;
    ring.ReadLatest([&](const ImageView& view, const shm::SlotHeader& slot) {
        // Reads slot f while writing slot f + 1: we are the only writer, so the
        // slot being read cannot change underneath us.
        WriteFrame(p, SlotContent(slot, view), static_cast<ColorSpace>(slot.colorspace), slot.timestamp, options);
    });
}

void PublishPending(vcam_producer* p)
{
    if (p->has_pending) {
        WriteFrame(p, p->pending_view, p->pending_colorspace, p->pending_timestamp, p->options);
        p->has_pending = false;
        std::vector<uint8_t>().swap(p->pending);
    }
}

void Worker(vcam_producer* p, std::promise<int> started)
{
    SetThreadDescription(GetCurrentThread(), L"vcam producer");
    // One producer per camera. The mutex belongs to this thread, which lives
    // exactly as long as the producer.
    HANDLE mutex = OpenOrCreateSharedMutex(ProducerMutexName(p->index));
    if (!mutex) {
        started.set_value(VCAM_ERROR_SYSTEM);
        return;
    }
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
        CloseHandle(mutex);
        started.set_value(VCAM_ERROR_BUSY);
        return;
    }
    started.set_value(VCAM_OK);

    int64_t next_attach = 0;
    for (;;) {
        HANDLE handles[2] = {p->stop_event, nullptr};
        DWORD count = 1;
        {
            std::lock_guard<std::mutex> lock(p->lock);
            if (p->shm.is_open() && p->shm.request_event()) {
                handles[count++] = p->shm.request_event();
            }
        }
        const DWORD r = WaitForMultipleObjects(count, handles, FALSE, kWorkerTick);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
            break;
        }
        std::lock_guard<std::mutex> lock(p->lock);
        const int64_t now = NowTicks();
        if (!p->shm.is_open() && now >= next_attach) {
            next_attach = now + kAttachRetry;
            if (p->shm.Open(p->index, true, p->capacity, p->allow_local)) {
                Log("producer: attached to camera %d", p->index + 1);
            }
        }
        if (p->shm.is_open()) {
            shm::Header* h = p->shm.ring().header();
            AtomicStore(h->producer_pid, static_cast<uint32_t>(GetCurrentProcessId()));
            AtomicStore(h->producer_flags, shm::kProducerAttached);
            AtomicStore(h->producer_heartbeat, now);
            PublishPending(p);
            Rerender(p);
        }
    }

    {
        std::lock_guard<std::mutex> lock(p->lock);
        if (p->shm.is_open()) {
            shm::Header* h = p->shm.ring().header();
            AtomicStore(h->producer_flags, 0u);
            AtomicStore(h->producer_pid, 0u);
            AtomicStore(h->producer_heartbeat, int64_t{0});
        }
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

bool BuildView(const vcam_frame* f, ImageView* view)
{
    if (!IsValidFormat(static_cast<uint32_t>(f->format)) || f->width < 1 || f->height < 1 ||
        f->width > static_cast<int32_t>(kMaxDimension) || f->height > static_cast<int32_t>(kMaxDimension)) {
        return false;
    }
    ImageView v;
    v.format = static_cast<PixelFormat>(f->format);
    v.width = f->width;
    v.height = f->height;
    for (int i = 0; i < PlaneCount(v.format); i++) {
        uint32_t row_bytes = 0, rows = 0;
        PlaneGeometry(v.format, static_cast<uint32_t>(v.width), static_cast<uint32_t>(v.height), i, &row_bytes, &rows);
        v.plane[i] = static_cast<const uint8_t*>(f->planes[i]);
        v.pitch[i] = f->pitches[i] ? f->pitches[i] : static_cast<int>(row_bytes);
    }
    *view = v;
    return IsUsable(v);
}

}  // namespace

extern "C" {

VCAM_API int VCAM_CALL vcam_api_version(void)
{
    return VCAM_API_VERSION;
}

VCAM_API const char* VCAM_CALL vcam_error_string(int result)
{
    switch (result) {
    case VCAM_OK: return "ok";
    case VCAM_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case VCAM_ERROR_BUSY: return "another producer is already feeding this camera";
    case VCAM_ERROR_NOT_INSTALLED: return "no such camera (is vcam installed? see `vcam status`)";
    case VCAM_ERROR_OUT_OF_MEMORY: return "out of memory";
    case VCAM_ERROR_SYSTEM: return "a system call failed";
    case VCAM_ERROR_TIMEOUT: return "timed out";
    default: return "unknown error";
    }
}

VCAM_API int VCAM_CALL vcam_open(const vcam_options* options, vcam_producer** producer)
{
    if (!producer) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }
    *producer = nullptr;
    vcam_options o = VCAM_OPTIONS_INIT;
    if (options) {
        if (options->struct_size < sizeof(vcam_options)) {
            return VCAM_ERROR_INVALID_ARGUMENT;
        }
        o = *options;
    }
    if (o.camera < 1 || o.camera > kMaxCameras) {
        return VCAM_ERROR_NOT_INSTALLED;
    }
    if (o.scale < VCAM_SCALE_FIT || o.scale > VCAM_SCALE_STRETCH) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }

    auto* p = new (std::nothrow) vcam_producer;
    if (!p) {
        return VCAM_ERROR_OUT_OF_MEMORY;
    }
    p->index = o.camera - 1;
    p->options.scale = static_cast<ScaleMode>(o.scale);
    p->options.mirror = o.mirror != 0;
    p->options.flip = o.flip != 0;
    p->options.background = Rgb{static_cast<uint8_t>(o.background >> 16), static_cast<uint8_t>(o.background >> 8),
                                static_cast<uint8_t>(o.background)};
    const Config config = LoadConfig();
    p->capacity = SlotCapacityFor(std::max<uint32_t>(config.max_width, 1920), std::max<uint32_t>(config.max_height, 1080));
    p->allow_local = config.backend == Backend::DirectShow;
    p->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!p->stop_event) {
        delete p;
        return VCAM_ERROR_SYSTEM;
    }

    std::promise<int> started;
    std::future<int> result = started.get_future();
    p->worker = std::thread(Worker, p, std::move(started));
    const int status = result.get();
    if (status != VCAM_OK) {
        p->worker.join();
        CloseHandle(p->stop_event);
        delete p;
        return status;
    }
    // Attach right away when possible, so the first frame goes straight out.
    {
        std::lock_guard<std::mutex> lock(p->lock);
        p->shm.Open(p->index, true, p->capacity, p->allow_local);
        if (p->shm.is_open()) {
            shm::Header* h = p->shm.ring().header();
            AtomicStore(h->producer_pid, static_cast<uint32_t>(GetCurrentProcessId()));
            AtomicStore(h->producer_flags, shm::kProducerAttached);
            AtomicStore(h->producer_heartbeat, NowTicks());
        }
    }
    *producer = p;
    return VCAM_OK;
}

VCAM_API int VCAM_CALL vcam_send(vcam_producer* p, const vcam_frame* frame)
{
    if (!p || !frame) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }
    ImageView view;
    if (!BuildView(frame, &view) || frame->colorspace < VCAM_COLORSPACE_BT601 ||
        frame->colorspace > VCAM_COLORSPACE_BT709_FULL) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }
    const auto colorspace = static_cast<ColorSpace>(frame->colorspace);
    std::lock_guard<std::mutex> lock(p->lock);
    if (p->shm.is_open()) {
        p->has_pending = false;
        return WriteFrame(p, view, colorspace, frame->timestamp, p->options) ? VCAM_OK : VCAM_ERROR_SYSTEM;
    }
    // Not attached yet: keep a copy to publish once the camera's memory exists.
    PlaneLayout layout;
    DefaultLayout(view.format, static_cast<uint32_t>(view.width), static_cast<uint32_t>(view.height), &layout);
    try {
        p->pending.resize(static_cast<size_t>(layout.total_bytes));
    } catch (const std::bad_alloc&) {
        return VCAM_ERROR_OUT_OF_MEMORY;
    }
    const ImageSpan copy = MakeSpan(view.format, view.width, view.height, p->pending.data());
    p->converter.Convert(view, copy, ConvertOptions{});
    p->pending_view = copy.view();
    p->pending_colorspace = colorspace;
    p->pending_timestamp = frame->timestamp;
    p->has_pending = true;
    return VCAM_OK;
}

VCAM_API int VCAM_CALL vcam_send_packed(vcam_producer* p, int32_t format, int32_t width, int32_t height,
                                        const void* data, int32_t pitch)
{
    if (!data || width < 1 || height < 1) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }
    vcam_frame f{};
    f.format = format;
    f.width = width;
    f.height = height;
    f.planes[0] = data;
    f.pitches[0] = pitch;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const int32_t luma_pitch = pitch ? pitch : width;
    if (format == VCAM_FORMAT_NV12) {
        f.planes[1] = bytes + static_cast<ptrdiff_t>(luma_pitch) * height;
        f.pitches[1] = luma_pitch;
    } else if (format == VCAM_FORMAT_I420) {
        const int32_t chroma_pitch = pitch ? pitch / 2 : (width + 1) / 2;
        f.planes[1] = bytes + static_cast<ptrdiff_t>(luma_pitch) * height;
        f.planes[2] = static_cast<const uint8_t*>(f.planes[1]) + static_cast<ptrdiff_t>(chroma_pitch) * ((height + 1) / 2);
        f.pitches[1] = f.pitches[2] = chroma_pitch;
    }
    return vcam_send(p, &f);
}

VCAM_API int VCAM_CALL vcam_get_status(vcam_producer* p, vcam_status* status)
{
    if (!p || !status || status->struct_size < sizeof(vcam_status)) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }
    vcam_status s{};
    s.struct_size = sizeof(vcam_status);
    s.frames_sent = p->frames_sent.load();
    std::lock_guard<std::mutex> lock(p->lock);
    if (p->shm.is_open()) {
        const shm::Header* h = p->shm.ring().header();
        s.connected = 1;
        s.consumer_active = ConsumerStreaming(h, NowTicks()) ? 1 : 0;
        shm::FrameRequest r;
        if (p->shm.ring().ReadRequest(&r) && RequestUsable(r)) {
            s.width = static_cast<int32_t>(r.width);
            s.height = static_cast<int32_t>(r.height);
            s.format = static_cast<int32_t>(r.format);
            s.fps_numerator = static_cast<int32_t>(r.fps_num);
            s.fps_denominator = static_cast<int32_t>(r.fps_den);
        }
        s.frames_delivered = AtomicLoad(h->frames_delivered, std::memory_order_relaxed);
        s.frames_repeated = AtomicLoad(h->frames_repeated, std::memory_order_relaxed);
    }
    *status = s;
    return VCAM_OK;
}

VCAM_API int VCAM_CALL vcam_wait_for_consumer(vcam_producer* p, int32_t timeout_ms)
{
    if (!p) {
        return VCAM_ERROR_INVALID_ARGUMENT;
    }
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(p->lock);
            if (p->shm.is_open() && ConsumerStreaming(p->shm.ring().header(), NowTicks())) {
                return VCAM_OK;
            }
        }
        if (timeout_ms >= 0 && GetTickCount64() >= deadline) {
            return VCAM_ERROR_TIMEOUT;
        }
        Sleep(20);
    }
}

VCAM_API void VCAM_CALL vcam_close(vcam_producer* p)
{
    if (!p) {
        return;
    }
    SetEvent(p->stop_event);
    if (p->worker.joinable()) {
        p->worker.join();
    }
    CloseHandle(p->stop_event);
    delete p;
}

}  // extern "C"
