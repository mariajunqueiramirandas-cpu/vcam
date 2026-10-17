#include "source.h"

#include <avrt.h>

#include <algorithm>

namespace vcam::win {

namespace {

constexpr DWORD kAllocatorSamples = 8;
constexpr int64_t kIdleWake = shm::kTicksPerSecond / 4;

const PixelFormat kOfferedFormats[] = {PixelFormat::NV12, PixelFormat::YUY2, PixelFormat::BGRA};

HRESULT SetStreamAttributes(IMFAttributes* a, DWORD id)
{
    HRESULT hr = a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, id);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    return hr;
}

HRESULT CreateVideoType(PixelFormat format, uint32_t width, uint32_t height, uint32_t fps, IMFMediaType** out)
{
    PlaneLayout layout;
    if (!DefaultLayout(format, width, height, &layout)) {
        return E_INVALIDARG;
    }
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, SubtypeFromFormat(format));
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(layout.total_bytes));
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_DEFAULT_STRIDE, layout.pitch[0]);
    if (SUCCEEDED(hr)) {
        const uint64_t bits = layout.total_bytes * 8 * fps;
        hr = t->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::min<uint64_t>(bits, 0xFFFFFFFFull)));
    }
    if (SUCCEEDED(hr) && IsYuv(format)) {
        // What we encode with; also what apps that ignore these attributes assume.
        hr = t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    }
    if (SUCCEEDED(hr)) {
        *out = t.Detach();
    }
    return hr;
}

// Reads what the pipeline negotiated. Any size the converter can produce is
// accepted, not only the advertised ones.
HRESULT ParseVideoType(IMFMediaType* type, PixelFormat* format, int* width, int* height, uint32_t* fps_num,
                       uint32_t* fps_den)
{
    GUID major = GUID_NULL, subtype = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
        FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return MF_E_INVALIDMEDIATYPE;
    }
    const PixelFormat f = FormatFromSubtype(subtype);
    UINT32 w = 0, h = 0;
    if (f == PixelFormat::Unknown || FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || w < 16 || h < 16 ||
        w > 4096 || h > 4096 || (IsYuv(f) && ((w | h) & 1))) {
        return MF_E_INVALIDMEDIATYPE;
    }
    UINT32 num = 0, den = 0;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) || num == 0 || den == 0 ||
        num / den > 240) {
        num = 30;
        den = 1;
    }
    *format = f;
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    *fps_num = num;
    *fps_den = den;
    return S_OK;
}

// Write access to a sample's buffer as an ImageSpan, with bounds checking.
class BufferLock {
public:
    BufferLock(IMFMediaBuffer* buffer, PixelFormat format, int width, int height)
        : buffer_(buffer), format_(format), width_(width), height_(height)
    {
        PlaneLayout layout;
        DefaultLayout(format, static_cast<uint32_t>(width), static_cast<uint32_t>(height), &layout);
        size_ = static_cast<DWORD>(layout.total_bytes);
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* start = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d2_)))) {
            hr_ = buffer2d2_->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &length);
            if (FAILED(hr_)) {
                buffer2d2_.Reset();
            }
        } else if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d_)))) {
            hr_ = buffer2d_->Lock2D(&scan0, &pitch);
            DWORD contiguous = 0;
            if (SUCCEEDED(hr_) && SUCCEEDED(buffer2d_->GetContiguousLength(&contiguous))) {
                length = contiguous;
                start = pitch >= 0 ? scan0 : scan0 + static_cast<ptrdiff_t>(pitch) * (height - 1);
            }
            if (FAILED(hr_)) {
                buffer2d_.Reset();
            }
        } else {
            DWORD max_length = 0, current = 0;
            hr_ = buffer->Lock(&scan0, &max_length, &current);
            plain_ = SUCCEEDED(hr_);
            start = scan0;
            length = max_length;
            pitch = static_cast<LONG>(layout.pitch[0]);
        }
        if (FAILED(hr_)) {
            return;
        }
        if (!Build(scan0, pitch, start, length)) {
            hr_ = MF_E_BUFFERTOOSMALL;
        }
    }

    ~BufferLock()
    {
        if (buffer2d2_) {
            buffer2d2_->Unlock2D();
        } else if (buffer2d_) {
            buffer2d_->Unlock2D();
        } else if (plain_) {
            buffer_->Unlock();
            buffer_->SetCurrentLength(size_);
        }
    }

    HRESULT hr() const { return hr_; }
    const ImageSpan& span() const { return span_; }

private:
    bool Build(BYTE* scan0, LONG pitch, BYTE* start, DWORD length)
    {
        span_.format = format_;
        span_.width = width_;
        span_.height = height_;
        span_.plane[0] = scan0;
        span_.pitch[0] = pitch;
        const ptrdiff_t luma_bytes = static_cast<ptrdiff_t>(pitch) * height_;
        if (format_ == PixelFormat::NV12) {
            if (pitch <= 0) {
                return false;
            }
            span_.plane[1] = scan0 + luma_bytes;
            span_.pitch[1] = pitch;
        } else if (format_ == PixelFormat::I420) {
            if (pitch <= 0) {
                return false;
            }
            span_.plane[1] = scan0 + luma_bytes;
            span_.pitch[1] = pitch / 2;
            span_.plane[2] = span_.plane[1] + static_cast<ptrdiff_t>(pitch / 2) * ((height_ + 1) / 2);
            span_.pitch[2] = pitch / 2;
        }
        if (!IsUsable(span_)) {
            return false;
        }
        if (!start || length == 0) {
            return true;  // size unknown; trust the allocator
        }
        // Every plane must lie inside [start, start + length).
        for (int p = 0; p < PlaneCount(format_); p++) {
            uint32_t row_bytes = 0, rows = 0;
            PlaneGeometry(format_, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), p, &row_bytes, &rows);
            const BYTE* first = span_.plane[p];
            const BYTE* last = span_.plane[p] + static_cast<ptrdiff_t>(span_.pitch[p]) * (rows - 1);
            const BYTE* lo = std::min(first, last);
            const BYTE* hi = std::max(first, last) + row_bytes;
            if (lo < start || hi > start + length) {
                return false;
            }
        }
        return true;
    }

    IMFMediaBuffer* buffer_;
    PixelFormat format_;
    int width_, height_;
    DWORD size_ = 0;
    ComPtr<IMF2DBuffer2> buffer2d2_;
    ComPtr<IMF2DBuffer> buffer2d_;
    bool plain_ = false;
    HRESULT hr_ = E_FAIL;
    ImageSpan span_;
};

}  // namespace

PixelFormat FormatFromSubtype(REFGUID subtype)
{
    if (subtype == MFVideoFormat_NV12) return PixelFormat::NV12;
    if (subtype == MFVideoFormat_YUY2) return PixelFormat::YUY2;
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) return PixelFormat::BGRA;
    if (subtype == MFVideoFormat_I420 || subtype == MFVideoFormat_IYUV) return PixelFormat::I420;
    if (subtype == MFVideoFormat_UYVY) return PixelFormat::UYVY;
    return PixelFormat::Unknown;
}

GUID SubtypeFromFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12: return MFVideoFormat_NV12;
    case PixelFormat::YUY2: return MFVideoFormat_YUY2;
    case PixelFormat::BGRA: return MFVideoFormat_RGB32;
    case PixelFormat::I420: return MFVideoFormat_I420;
    case PixelFormat::UYVY: return MFVideoFormat_UYVY;
    default: return GUID_NULL;
    }
}

// --- construction ------------------------------------------------------------------

MediaStream::MediaStream(MediaSource* source, DWORD id, int camera)
    : source_(static_cast<IMFMediaSourceEx*>(source)), id_(id), camera_(camera), feed_(camera)
{
    g_objects++;
}

MediaStream::~MediaStream()
{
    StopWorker();
    if (stop_event_) CloseHandle(stop_event_);
    if (work_event_) CloseHandle(work_event_);
    g_objects--;
}

HRESULT MediaStream::Create(MediaSource* source, DWORD id, int camera, const std::vector<VideoMode>& modes,
                            MediaStream** stream)
{
    MediaStream* s = new (std::nothrow) MediaStream(source, id, camera);
    if (!s) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = s->Initialize(modes);
    if (FAILED(hr)) {
        s->Shutdown();
        s->Release();
        return hr;
    }
    *stream = s;
    return S_OK;
}

HRESULT MediaStream::Initialize(const std::vector<VideoMode>& modes)
{
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    work_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event_ || !work_event_) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes_, 4);
    if (SUCCEEDED(hr)) hr = SetStreamAttributes(attributes_.Get(), id_);
    if (FAILED(hr)) {
        return hr;
    }

    std::vector<ComPtr<IMFMediaType>> types;
    for (const VideoMode& mode : modes) {
        for (PixelFormat format : kOfferedFormats) {
            ComPtr<IMFMediaType> type;
            hr = CreateVideoType(format, mode.width, mode.height, mode.fps, &type);
            if (FAILED(hr)) {
                return hr;
            }
            types.push_back(type);
        }
    }
    std::vector<IMFMediaType*> list;
    for (auto& t : types) {
        list.push_back(t.Get());
    }
    hr = MFCreateStreamDescriptor(id_, static_cast<DWORD>(list.size()), list.data(), &descriptor_);
    ComPtr<IMFMediaTypeHandler> handler;
    if (SUCCEEDED(hr)) hr = descriptor_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(list[0]);
    if (SUCCEEDED(hr)) hr = SetStreamAttributes(descriptor_.Get(), id_);
    return hr;
}

// --- IUnknown ----------------------------------------------------------------------------

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
        riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef()
{
    return ++refs_;
}

STDMETHODIMP_(ULONG) MediaStream::Release()
{
    const ULONG n = --refs_;
    if (n == 0) {
        delete this;
    }
    return n;
}

// --- IMFMediaEventGenerator -----------------------------------------------------------------

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) {
            return MF_E_SHUTDOWN;
        }
        queue = queue_;
    }
    return queue->GetEvent(flags, event);  // may block: not under the lock
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value)
{
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(type, extended, status, value);
}

// --- IMFMediaStream -----------------------------------------------------------------------------

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source)
{
    if (!source) {
        return E_POINTER;
    }
    *source = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : source_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor)
{
    if (!descriptor) {
        return E_POINTER;
    }
    *descriptor = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : descriptor_.CopyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token)
{
    std::lock_guard<std::mutex> lock(lock_);
    if (shutdown_) {
        return MF_E_SHUTDOWN;
    }
    if (state_ != MF_STREAM_STATE_RUNNING) {
        return MF_E_INVALIDREQUEST;
    }
    tokens_.emplace_back(token);
    SetEvent(work_event_);
    return S_OK;
}

// --- IMFMediaStream2 ----------------------------------------------------------------------------

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) {
            return MF_E_SHUTDOWN;
        }
        if (value == state_) {
            return S_OK;
        }
        if (value == MF_STREAM_STATE_PAUSED) {
            if (state_ != MF_STREAM_STATE_RUNNING) {
                return MF_E_INVALID_STATE_TRANSITION;
            }
            state_ = MF_STREAM_STATE_PAUSED;  // requests are refused until it runs again
            return S_OK;
        }
    }
    switch (value) {
    case MF_STREAM_STATE_RUNNING: return Begin(nullptr);
    case MF_STREAM_STATE_STOPPED: return Stop(false);
    default: return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state)
{
    if (!state) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(lock_);
    if (shutdown_) {
        return MF_E_SHUTDOWN;
    }
    *state = state_;
    return S_OK;
}

// --- control from the source -----------------------------------------------------------------------

HRESULT MediaStream::Start(IMFMediaType* type)
{
    if (!type) {
        return E_INVALIDARG;
    }
    HRESULT hr = Begin(type);
    if (FAILED(hr)) {
        return hr;
    }
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Begin(IMFMediaType* requested)
{
    ComPtr<IMFMediaType> type = requested;
    if (!type) {
        std::lock_guard<std::mutex> lock(lock_);
        type = type_;
        if (!type && descriptor_) {
            ComPtr<IMFMediaTypeHandler> handler;
            if (SUCCEEDED(descriptor_->GetMediaTypeHandler(&handler))) {
                handler->GetCurrentMediaType(&type);
            }
        }
        if (!type) {
            return MF_E_NOT_INITIALIZED;
        }
    }
    PixelFormat format;
    int width, height;
    uint32_t fps_num, fps_den;
    HRESULT hr = ParseVideoType(type.Get(), &format, &width, &height, &fps_num, &fps_den);
    if (FAILED(hr)) {
        Log("camera %d: rejected media type (0x%08lX)", camera_ + 1, static_cast<unsigned long>(hr));
        return hr;
    }

    StopWorker();  // restarted below with the new configuration
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) {
            return MF_E_SHUTDOWN;
        }
        if (!allocator_) {
            // Nobody provided an allocator (e.g. the source is used in-process): make our own.
            ComPtr<IMFVideoSampleAllocatorEx> own;
            hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&own));
            if (FAILED(hr)) {
                return hr;
            }
            allocator_ = own;
            allocator_ready_ = false;
        }
        BOOL same = FALSE;
        if (allocator_ready_ && type_) {
            DWORD flags = 0;
            same = type_->IsEqual(type.Get(), &flags) == S_OK;
        }
        if (!same) {
            if (allocator_ready_) {
                allocator_->UninitializeSampleAllocator();
                allocator_ready_ = false;
            }
            hr = allocator_->InitializeSampleAllocator(kAllocatorSamples, type.Get());
            if (FAILED(hr)) {
                Log("camera %d: InitializeSampleAllocator failed (0x%08lX)", camera_ + 1, static_cast<unsigned long>(hr));
                return hr;
            }
            allocator_ready_ = true;
            type_ = type;
        }
        format_ = format;
        width_ = width;
        height_ = height;
        fps_num_ = fps_num;
        fps_den_ = fps_den;
        interval_ = std::max<int64_t>(10000, static_cast<int64_t>(fps_den) * shm::kTicksPerSecond / fps_num);
        state_ = MF_STREAM_STATE_RUNNING;
    }
    feed_.Configure(format, width, height, fps_num, fps_den);
    StartWorker();
    Log("camera %d: streaming %dx%d %s at %u/%u fps", camera_ + 1, width, height, FormatName(format), fps_num, fps_den);
    return S_OK;
}

HRESULT MediaStream::Stop(bool send_event)
{
    StopWorker();
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_) {
            return MF_E_SHUTDOWN;
        }
        state_ = MF_STREAM_STATE_STOPPED;
        tokens_.clear();
        queue = queue_;
    }
    feed_.Stop();
    Log("camera %d: stream stopped", camera_ + 1);
    return send_event ? queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr) : S_OK;
}

void MediaStream::Shutdown()
{
    StopWorker();
    std::lock_guard<std::mutex> lock(lock_);
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    state_ = MF_STREAM_STATE_STOPPED;
    tokens_.clear();
    if (queue_) {
        queue_->Shutdown();
    }
    if (allocator_ && allocator_ready_) {
        allocator_->UninitializeSampleAllocator();
    }
    feed_.Stop();
    queue_.Reset();
    descriptor_.Reset();
    attributes_.Reset();
    allocator_.Reset();
    type_.Reset();
    source_.Reset();  // breaks the source <-> stream reference cycle
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
    if (!allocator) {
        return E_POINTER;
    }
    ComPtr<IMFVideoSampleAllocator> a;
    HRESULT hr = allocator->QueryInterface(IID_PPV_ARGS(&a));
    if (FAILED(hr)) {
        return hr;
    }
    std::lock_guard<std::mutex> lock(lock_);
    if (shutdown_) {
        return MF_E_SHUTDOWN;
    }
    if (state_ == MF_STREAM_STATE_RUNNING) {
        return MF_E_INVALIDREQUEST;  // cannot swap allocators while streaming
    }
    if (allocator_ && allocator_ready_) {
        allocator_->UninitializeSampleAllocator();
    }
    allocator_ = a;
    allocator_ready_ = false;
    return S_OK;
}

HRESULT MediaStream::GetAttributes(IMFAttributes** attributes)
{
    std::lock_guard<std::mutex> lock(lock_);
    return shutdown_ ? MF_E_SHUTDOWN : attributes_.CopyTo(attributes);
}

// --- frame pump ----------------------------------------------------------------------------------------

void MediaStream::StartWorker()
{
    if (worker_.joinable()) {
        return;
    }
    ResetEvent(stop_event_);
    worker_ = std::thread([this] { Worker(); });
}

void MediaStream::StopWorker()
{
    if (worker_.joinable()) {
        SetEvent(stop_event_);
        worker_.join();
    }
}

void MediaStream::Worker()
{
    SetThreadDescription(GetCurrentThread(), L"vcam frame pump");
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    FramePacer pacer;
    pacer.Start(interval_, NowTicks());
    for (;;) {
        const int64_t now = NowTicks();
        feed_.Tick(now);
        int64_t wake = now + kIdleWake;
        bool pending;
        {
            std::lock_guard<std::mutex> lock(lock_);
            pending = !tokens_.empty() && state_ == MF_STREAM_STATE_RUNNING;
        }
        if (pending) {
            int64_t when = 0;
            if (pacer.Poll(now, feed_.latest(), feed_.producer_alive(now), &when)) {
                uint64_t frame = 0;
                const HRESULT hr = Deliver(now, &frame);
                if (SUCCEEDED(hr)) {
                    pacer.Delivered(now, frame);
                    continue;
                }
                if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
                    wake = now + 20000;  // the pipeline still holds every sample; try again shortly
                } else if (hr != MF_E_INVALIDREQUEST && hr != MF_E_SHUTDOWN) {
                    Log("camera %d: delivering a sample failed (0x%08lX)", camera_ + 1, static_cast<unsigned long>(hr));
                    wake = now + interval_;
                }
            } else {
                wake = std::min(wake, when);
            }
        }

        HANDLE handles[4] = {stop_event_, work_event_, nullptr, nullptr};
        DWORD count = 2;
        DWORD timeout = INFINITE;
        const int64_t delay = std::max<int64_t>(0, wake - NowTicks());
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -delay;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            handles[count++] = timer;
        } else {
            timeout = static_cast<DWORD>((delay + 9999) / 10000);
        }
        if (HANDLE frames = feed_.frame_event()) {
            handles[count++] = frames;
        }
        const DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
            break;
        }
    }

    if (timer) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
}

HRESULT MediaStream::Deliver(int64_t now, uint64_t* frame)
{
    ComPtr<IMFVideoSampleAllocator> allocator;
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_ || state_ != MF_STREAM_STATE_RUNNING || tokens_.empty() || !allocator_ready_) {
            return MF_E_INVALIDREQUEST;
        }
        allocator = allocator_;
        queue = queue_;
    }
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator->AllocateSample(&sample);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        return hr;
    }
    {
        BufferLock locked(buffer.Get(), format_, width_, height_);
        if (FAILED(locked.hr())) {
            return locked.hr();
        }
        *frame = feed_.Render(locked.span(), now);
    }
    hr = sample->SetSampleTime(MFGetSystemTime());
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(interval_);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IUnknown> token;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shutdown_ || state_ != MF_STREAM_STATE_RUNNING || tokens_.empty()) {
            return MF_E_INVALIDREQUEST;  // stopped while we were rendering
        }
        token = tokens_.front();
        tokens_.pop_front();
    }
    if (token) {
        sample->SetUnknown(MFSampleExtension_Token, token.Get());
    }
    hr = queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    if (SUCCEEDED(hr)) {
        feed_.CountDelivery(*frame != 0 && *frame != last_frame_sent_);
        last_frame_sent_ = *frame;
    }
    return hr;
}

}  // namespace vcam::win
