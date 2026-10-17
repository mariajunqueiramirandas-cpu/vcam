// The capture pin: connection and allocator negotiation, the formats it
// offers (IAMStreamConfig), the capture category (IKsPropertySet) and the
// thread that delivers samples.
#include "dshow.h"

#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "vcam/pacer.h"

namespace vcam::win {

namespace {

constexpr wchar_t kPinName[] = L"Capture";
constexpr int64_t kIdleWake = shm::kTicksPerSecond / 10;  // heartbeat and state checks while idle
constexpr DWORD kSupportGet = 1;                          // KSPROPERTY_SUPPORT_GET

class MediaTypeEnum final : public IEnumMediaTypes {
public:
    MediaTypeEnum(std::vector<MediaType> types, ULONG position) : types_(std::move(types)), position_(position)
    {
        g_objects++;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs_;
        if (n == 0) {
            delete this;
        }
        return n;
    }

    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override
    {
        if (!types || (count > 1 && !fetched)) {
            return E_POINTER;
        }
        ULONG n = 0;
        while (n < count && position_ < types_.size()) {
            types[n] = types_[position_].CreateCopy();
            if (!types[n]) {
                break;
            }
            n++;
            position_++;
        }
        if (fetched) {
            *fetched = n;
        }
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        position_ += count;
        if (position_ > types_.size()) {
            position_ = static_cast<ULONG>(types_.size());
            return S_FALSE;
        }
        return S_OK;
    }
    STDMETHODIMP Reset() override
    {
        position_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** clone) override
    {
        if (!clone) {
            return E_POINTER;
        }
        *clone = new (std::nothrow) MediaTypeEnum(types_, position_);
        return *clone ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~MediaTypeEnum() { g_objects--; }

    std::atomic<ULONG> refs_{1};
    std::vector<MediaType> types_;
    ULONG position_;
};

// The frame rate as a fraction, exact for the usual rates.
void RateOf(int64_t interval, uint32_t* num, uint32_t* den)
{
    const double fps = 1e7 / static_cast<double>(interval);
    const double rounded = std::round(fps);
    if (rounded >= 1 && std::fabs(fps - rounded) < rounded * 0.001) {
        *num = static_cast<uint32_t>(rounded);
        *den = 1;
    } else {
        *num = 10'000'000;
        *den = static_cast<uint32_t>(interval);
    }
}

// The sample's memory as an image of `format`.
ImageSpan SpanFor(const VideoFormat& format, BYTE* data)
{
    ImageSpan s;
    s.format = format.format;
    s.width = format.width;
    s.height = format.height;
    s.plane[0] = data;
    s.pitch[0] = format.stride;
    const ptrdiff_t luma = static_cast<ptrdiff_t>(format.stride) * format.height;
    if (format.format == PixelFormat::NV12) {
        s.plane[1] = data + luma;
        s.pitch[1] = format.stride;
    } else if (format.format == PixelFormat::I420) {
        s.plane[1] = data + luma;
        s.pitch[1] = format.stride / 2;
        s.plane[2] = s.plane[1] + static_cast<ptrdiff_t>(format.stride / 2) * (format.height / 2);
        s.pitch[2] = format.stride / 2;
    }
    return s;
}

bool Matches(const AM_MEDIA_TYPE& mt, const AM_MEDIA_TYPE* partial)
{
    if (!partial) {
        return true;
    }
    return (partial->majortype == GUID_NULL || partial->majortype == mt.majortype) &&
           (partial->subtype == GUID_NULL || partial->subtype == mt.subtype) &&
           (partial->formattype == GUID_NULL || partial->formattype == mt.formattype);
}

}  // namespace

OutputPin::OutputPin(Filter* filter, int camera)
    : filter_(filter), camera_(camera), config_(LoadConfig()), offered_(OfferedTypes(config_)), feed_(camera, true)
{
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

OutputPin::~OutputPin()
{
    Inactive();
    ReleaseConnection();
    for (HANDLE h : {stop_event_, wake_event_}) {
        if (h) {
            CloseHandle(h);
        }
    }
}

STDMETHODIMP OutputPin::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else if (riid == IID_IAMPushSource || riid == IID_IAMLatency) {
        *ppv = static_cast<IAMPushSource*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) OutputPin::AddRef()
{
    return filter_->AddRef();
}

STDMETHODIMP_(ULONG) OutputPin::Release()
{
    return filter_->Release();
}

// --- connection ----------------------------------------------------------------------------

MediaType OutputPin::DefaultType() const
{
    if (offered_.empty()) {
        return MakeVideoType(PixelFormat::NV12, 640, 480, 30);
    }
    const OfferedType& t = offered_.front();
    return MakeVideoType(t.format, t.mode.width, t.mode.height, t.mode.fps);
}

std::vector<MediaType> OutputPin::Candidates() const
{
    std::vector<MediaType> types;
    if (!preferred_.empty()) {
        types.push_back(preferred_);
        return types;
    }
    types.reserve(offered_.size());
    for (const OfferedType& t : offered_) {
        types.push_back(MakeVideoType(t.format, t.mode.width, t.mode.height, t.mode.fps));
    }
    return types;
}

bool OutputPin::Accepts(const AM_MEDIA_TYPE& mt) const
{
    VideoFormat f;
    if (!ParseVideoType(mt, &f)) {
        return false;
    }
    if (!preferred_.empty()) {
        // After SetFormat only that format (any stride) will do.
        VideoFormat p;
        return ParseVideoType(preferred_.get(), &p) && SameVideo(f, p) && f.interval == p.interval;
    }
    return true;
}

STDMETHODIMP OutputPin::Connect(IPin* receive, const AM_MEDIA_TYPE* pmt)
{
    if (!receive) {
        return E_POINTER;
    }
    std::lock_guard<std::recursive_mutex> lock(filter_->state_lock());
    if (connected_) {
        return VFW_E_ALREADY_CONNECTED;
    }
    if (!filter_->stopped()) {
        return VFW_E_NOT_STOPPED;
    }
    // A complete type: that one or nothing.
    if (pmt && pmt->majortype != GUID_NULL && pmt->subtype != GUID_NULL && pmt->formattype != GUID_NULL &&
        pmt->pbFormat) {
        return Accepts(*pmt) ? TryConnect(receive, *pmt) : VFW_E_TYPE_NOT_ACCEPTED;
    }
    // Our formats first, then whatever the other side proposes that we can make.
    for (const MediaType& t : Candidates()) {
        if (Matches(t.get(), pmt) && SUCCEEDED(TryConnect(receive, t.get()))) {
            return S_OK;
        }
    }
    IEnumMediaTypes* types = nullptr;
    if (SUCCEEDED(receive->EnumMediaTypes(&types)) && types) {
        AM_MEDIA_TYPE* proposed = nullptr;
        ULONG fetched = 0;
        bool connected = false;
        while (!connected && types->Next(1, &proposed, &fetched) == S_OK && proposed) {
            if (Matches(*proposed, pmt) && Accepts(*proposed)) {
                connected = SUCCEEDED(TryConnect(receive, *proposed));
            }
            DeleteMediaType(proposed);
            proposed = nullptr;
        }
        types->Release();
        if (connected) {
            return S_OK;
        }
    }
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT OutputPin::TryConnect(IPin* receive, const AM_MEDIA_TYPE& mt)
{
    VideoFormat format;
    if (!ParseVideoType(mt, &format)) {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    {
        std::lock_guard<std::mutex> lock(type_lock_);
        connected_ = receive;
        connected_->AddRef();
        format_ = format;
        mt_ = MediaType(mt);
    }
    HRESULT hr = receive->ReceiveConnection(static_cast<IPin*>(this), &mt);
    if (SUCCEEDED(hr)) {
        hr = DecideAllocator(receive);
        if (FAILED(hr)) {
            receive->Disconnect();
        }
    }
    if (FAILED(hr)) {
        ReleaseConnection();
        return hr;
    }
    Log("camera %d: connected, %dx%d %s, %.4g fps", camera_ + 1, format.width, format.height,
        FormatName(format.format), 1e7 / static_cast<double>(format.interval));
    return S_OK;
}

HRESULT OutputPin::DecideAllocator(IPin* receive)
{
    IMemInputPin* input = nullptr;
    HRESULT hr = receive->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input));
    if (FAILED(hr) || !input) {
        return VFW_E_NO_TRANSPORT;
    }
    ALLOCATOR_PROPERTIES wanted;
    memset(&wanted, 0, sizeof(wanted));
    input->GetAllocatorRequirements(&wanted);  // E_NOTIMPL is fine

    const auto configure = [&](IMemAllocator* allocator) {
        ALLOCATOR_PROPERTIES props = wanted;
        props.cBuffers = std::max<long>(wanted.cBuffers, 3);
        props.cbBuffer = std::max<long>(wanted.cbBuffer, static_cast<long>(format_.image_bytes));
        props.cbAlign = std::max<long>(wanted.cbAlign, 1);
        ALLOCATOR_PROPERTIES actual;
        memset(&actual, 0, sizeof(actual));
        HRESULT result = allocator->SetProperties(&props, &actual);
        if (SUCCEEDED(result) && actual.cbBuffer < static_cast<long>(format_.image_bytes)) {
            result = E_FAIL;
        }
        if (SUCCEEDED(result)) {
            result = input->NotifyAllocator(allocator, FALSE);
        }
        return result;
    };

    // The downstream pin's allocator if it has one (renderers do), else our own.
    IMemAllocator* allocator = nullptr;
    hr = input->GetAllocator(&allocator);
    if (SUCCEEDED(hr) && allocator) {
        hr = configure(allocator);
        if (FAILED(hr)) {
            allocator->Release();
            allocator = nullptr;
        }
    }
    if (!allocator) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                              reinterpret_cast<void**>(&allocator));
        if (SUCCEEDED(hr)) {
            hr = configure(allocator);
            if (FAILED(hr)) {
                allocator->Release();
                allocator = nullptr;
            }
        }
    }
    if (FAILED(hr)) {
        input->Release();
        return hr;
    }
    input_ = input;
    allocator_ = allocator;
    return S_OK;
}

void OutputPin::ReleaseConnection()
{
    if (allocator_) {
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (input_) {
        input_->Release();
        input_ = nullptr;
    }
    IPin* connected = nullptr;
    {
        std::lock_guard<std::mutex> lock(type_lock_);
        connected = connected_;
        connected_ = nullptr;
        format_ = VideoFormat{};
        mt_ = MediaType();
    }
    if (connected) {
        connected->Release();
    }
}

STDMETHODIMP OutputPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*)
{
    return E_UNEXPECTED;  // an output pin initiates connections
}

STDMETHODIMP OutputPin::Disconnect()
{
    std::lock_guard<std::recursive_mutex> lock(filter_->state_lock());
    if (!filter_->stopped()) {
        return VFW_E_NOT_STOPPED;
    }
    if (!connected_) {
        return S_FALSE;
    }
    if (allocator_) {
        allocator_->Decommit();
    }
    ReleaseConnection();
    return S_OK;
}

STDMETHODIMP OutputPin::ConnectedTo(IPin** pin)
{
    if (!pin) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(type_lock_);
    *pin = connected_;
    if (!connected_) {
        return VFW_E_NOT_CONNECTED;
    }
    connected_->AddRef();
    return S_OK;
}

STDMETHODIMP OutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(type_lock_);
    if (mt_.empty()) {
        memset(pmt, 0, sizeof(*pmt));
        return VFW_E_NOT_CONNECTED;
    }
    return CopyMediaType(pmt, mt_.get());
}

STDMETHODIMP OutputPin::QueryPinInfo(PIN_INFO* info)
{
    if (!info) {
        return E_POINTER;
    }
    info->pFilter = filter_;
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcsncpy_s(info->achName, MAX_PIN_NAME, kPinName, _TRUNCATE);
    return S_OK;
}

STDMETHODIMP OutputPin::QueryDirection(PIN_DIRECTION* direction)
{
    if (!direction) {
        return E_POINTER;
    }
    *direction = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP OutputPin::QueryId(LPWSTR* id)
{
    if (!id) {
        return E_POINTER;
    }
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinName)));
    if (!*id) {
        return E_OUTOFMEMORY;
    }
    memcpy(*id, kPinName, sizeof(kPinName));
    return S_OK;
}

STDMETHODIMP OutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    if (!pmt) {
        return E_POINTER;
    }
    VideoFormat f;
    if (!ParseVideoType(*pmt, &f)) {
        return S_FALSE;
    }
    {
        // Downstream asks whether it may change the stride of the connection;
        // a renderer may ask while we stream, so without the state lock.
        std::lock_guard<std::mutex> lock(type_lock_);
        if (connected_) {
            return SameVideo(f, format_) ? S_OK : S_FALSE;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(filter_->state_lock());
    return Accepts(*pmt) ? S_OK : S_FALSE;
}

STDMETHODIMP OutputPin::EnumMediaTypes(IEnumMediaTypes** types)
{
    if (!types) {
        return E_POINTER;
    }
    std::lock_guard<std::recursive_mutex> lock(filter_->state_lock());
    *types = new (std::nothrow) MediaTypeEnum(Candidates(), 0);
    return *types ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OutputPin::QueryInternalConnections(IPin**, ULONG*)
{
    return E_NOTIMPL;
}

STDMETHODIMP OutputPin::EndOfStream()
{
    return E_UNEXPECTED;
}

STDMETHODIMP OutputPin::BeginFlush()
{
    return E_UNEXPECTED;
}

STDMETHODIMP OutputPin::EndFlush()
{
    return E_UNEXPECTED;
}

STDMETHODIMP OutputPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double)
{
    return E_UNEXPECTED;
}

// --- IAMStreamConfig -----------------------------------------------------------------------------

STDMETHODIMP OutputPin::SetFormat(AM_MEDIA_TYPE* pmt)
{
    std::lock_guard<std::recursive_mutex> lock(filter_->state_lock());
    if (!pmt) {
        preferred_ = MediaType();
        return S_OK;
    }
    VideoFormat f;
    if (!ParseVideoType(*pmt, &f)) {
        return VFW_E_INVALIDMEDIATYPE;
    }
    if (!filter_->stopped()) {
        return VFW_E_NOT_STOPPED;
    }
    const MediaType previous = preferred_;
    preferred_ = MediaType(*pmt);
    if (!connected_) {
        return S_OK;
    }
    GUID current_subtype = GUID_NULL;
    {
        std::lock_guard<std::mutex> type_lock(type_lock_);
        current_subtype = mt_.get().subtype;
    }
    if (SameVideo(f, format_) && f.interval == format_.interval && pmt->subtype == current_subtype) {
        return S_OK;
    }
    // Connected with another format: the graph reconnects the pins, which
    // then agree on the new one.
    IFilterGraph* graph = filter_->filter_graph();
    if (connected_->QueryAccept(pmt) != S_OK || !graph) {
        preferred_ = previous;
        return graph ? VFW_E_INVALIDMEDIATYPE : VFW_E_NOT_IN_GRAPH;
    }
    IFilterGraph2* graph2 = nullptr;
    HRESULT hr;
    if (SUCCEEDED(graph->QueryInterface(IID_IFilterGraph2, reinterpret_cast<void**>(&graph2))) && graph2) {
        hr = graph2->ReconnectEx(static_cast<IPin*>(this), pmt);
        graph2->Release();
    } else {
        hr = graph->Reconnect(static_cast<IPin*>(this));
    }
    return hr;
}

STDMETHODIMP OutputPin::GetFormat(AM_MEDIA_TYPE** pmt)
{
    if (!pmt) {
        return E_POINTER;
    }
    std::lock_guard<std::recursive_mutex> lock(filter_->state_lock());
    MediaType current;
    {
        std::lock_guard<std::mutex> type_lock(type_lock_);
        current = mt_;
    }
    if (current.empty()) {
        current = preferred_.empty() ? DefaultType() : preferred_;
    }
    *pmt = current.CreateCopy();
    return *pmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OutputPin::GetNumberOfCapabilities(int* count, int* size)
{
    if (!count || !size) {
        return E_POINTER;
    }
    *count = static_cast<int>(offered_.size());
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP OutputPin::GetStreamCaps(int index, AM_MEDIA_TYPE** pmt, BYTE* caps)
{
    if (!pmt || !caps) {
        return E_POINTER;
    }
    if (index < 0) {
        return E_INVALIDARG;
    }
    if (static_cast<size_t>(index) >= offered_.size()) {
        return S_FALSE;
    }
    const OfferedType& t = offered_[static_cast<size_t>(index)];
    const MediaType type = MakeVideoType(t.format, t.mode.width, t.mode.height, t.mode.fps);
    *pmt = type.CreateCopy();
    if (!*pmt) {
        return E_OUTOFMEMORY;
    }
    VideoFormat f;
    ParseVideoType(type.get(), &f);
    VIDEO_STREAM_CONFIG_CAPS c;
    memset(&c, 0, sizeof(c));
    c.guid = FORMAT_VideoInfo;
    c.VideoStandard = AnalogVideo_None;
    const SIZE size{static_cast<LONG>(t.mode.width), static_cast<LONG>(t.mode.height)};
    c.InputSize = size;
    c.MinCroppingSize = size;
    c.MaxCroppingSize = size;
    c.CropGranularityX = 1;
    c.CropGranularityY = 1;
    c.CropAlignX = 1;
    c.CropAlignY = 1;
    c.MinOutputSize = size;
    c.MaxOutputSize = size;
    c.OutputGranularityX = 1;
    c.OutputGranularityY = 1;
    c.MinFrameInterval = f.interval;
    c.MaxFrameInterval = f.interval;
    const LONG bits = static_cast<LONG>(std::min<uint64_t>(static_cast<uint64_t>(f.image_bytes) * 8 * t.mode.fps,
                                                            0x7fffffff));
    c.MinBitsPerSecond = bits;
    c.MaxBitsPerSecond = bits;
    memcpy(caps, &c, sizeof(c));
    return S_OK;
}

// --- IKsPropertySet -------------------------------------------------------------------------------

STDMETHODIMP OutputPin::Set(REFGUID set, DWORD, LPVOID, DWORD, LPVOID, DWORD)
{
    // The category is read-only, and the only property in the set.
    return set == AMPROPSETID_Pin ? E_PROP_ID_UNSUPPORTED : E_PROP_SET_UNSUPPORTED;
}

STDMETHODIMP OutputPin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD data_bytes, DWORD* returned)
{
    if (set != AMPROPSETID_Pin) {
        return E_PROP_SET_UNSUPPORTED;
    }
    if (id != static_cast<DWORD>(AMPROPERTY_PIN_CATEGORY)) {
        return E_PROP_ID_UNSUPPORTED;
    }
    if (!data && !returned) {
        return E_POINTER;
    }
    if (returned) {
        *returned = sizeof(GUID);
    }
    if (!data) {
        return S_OK;  // the caller only asked for the size
    }
    if (data_bytes < sizeof(GUID)) {
        return E_UNEXPECTED;
    }
    memcpy(data, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
    return S_OK;
}

STDMETHODIMP OutputPin::QuerySupported(REFGUID set, DWORD id, DWORD* support)
{
    if (set != AMPROPSETID_Pin) {
        return E_PROP_SET_UNSUPPORTED;
    }
    if (id != static_cast<DWORD>(AMPROPERTY_PIN_CATEGORY)) {
        return E_PROP_ID_UNSUPPORTED;
    }
    if (support) {
        *support = kSupportGet;
    }
    return S_OK;
}

// --- IAMPushSource ------------------------------------------------------------------------------------

STDMETHODIMP OutputPin::GetLatency(REFERENCE_TIME* latency)
{
    if (!latency) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(type_lock_);
    *latency = format_.interval ? format_.interval : 333333;  // one frame
    return S_OK;
}

STDMETHODIMP OutputPin::GetPushSourceFlags(ULONG* flags)
{
    if (!flags) {
        return E_POINTER;
    }
    *flags = 0;  // timestamps are stream times of the graph clock
    return S_OK;
}

STDMETHODIMP OutputPin::SetPushSourceFlags(ULONG)
{
    return E_NOTIMPL;
}

STDMETHODIMP OutputPin::SetStreamOffset(REFERENCE_TIME offset)
{
    stream_offset_ = offset;
    return S_OK;
}

STDMETHODIMP OutputPin::GetStreamOffset(REFERENCE_TIME* offset)
{
    if (!offset) {
        return E_POINTER;
    }
    *offset = stream_offset_.load();
    return S_OK;
}

STDMETHODIMP OutputPin::GetMaxStreamOffset(REFERENCE_TIME* offset)
{
    if (!offset) {
        return E_POINTER;
    }
    *offset = max_stream_offset_.load();
    return S_OK;
}

STDMETHODIMP OutputPin::SetMaxStreamOffset(REFERENCE_TIME offset)
{
    max_stream_offset_ = offset;
    return S_OK;
}

// --- streaming ----------------------------------------------------------------------------------------

HRESULT OutputPin::Active()
{
    if (!connected_ || worker_.joinable()) {
        return S_OK;  // an unconnected pin has nothing to stream
    }
    if (!allocator_ || !input_) {
        return VFW_E_NO_ALLOCATOR;
    }
    const HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) {
        return hr;
    }
    stream_input_ = input_;
    stream_input_->AddRef();
    stream_allocator_ = allocator_;
    stream_allocator_->AddRef();
    // The connection type, with the stride a renderer may have asked for since.
    stream_format_ = format_;
    {
        std::lock_guard<std::mutex> lock(type_lock_);
        VideoFormat current;
        if (ParseVideoType(mt_.get(), &current) && SameVideo(current, format_)) {
            current.interval = format_.interval;
            stream_format_ = current;
        }
    }
    uint32_t num = 0, den = 0;
    RateOf(format_.interval, &num, &den);
    feed_.Configure(format_.format, format_.width, format_.height, num, den);
    delivered_ = 0;
    last_frame_sent_ = 0;
    last_start_ = 0;
    ResetEvent(stop_event_);
    worker_ = std::thread([this] { Worker(); });
    return S_OK;
}

void OutputPin::Inactive()
{
    if (!worker_.joinable()) {
        return;
    }
    SetEvent(stop_event_);
    // GetBuffer returns at once on a decommitted allocator.
    stream_allocator_->Decommit();
    worker_.join();
    feed_.Stop();
    stream_allocator_->Release();
    stream_allocator_ = nullptr;
    stream_input_->Release();
    stream_input_ = nullptr;
}

void OutputPin::Wake()
{
    SetEvent(wake_event_);
}

void OutputPin::Worker()
{
    SetThreadDescription(GetCurrentThread(), L"vcam DirectShow capture");
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    VideoFormat format = stream_format_;
    FramePacer pacer;
    bool delivering = false, first = true, failed = false;
    for (;;) {
        const int64_t now = NowTicks();
        feed_.Tick(now);
        int64_t wake = now + kIdleWake;
        if (!filter_->running()) {
            delivering = false;
        } else if (!delivering) {
            // (Re)started. The graph starts the stream a little in the future;
            // the first sample waits for it, so stream times start at zero.
            const int64_t until_start = TimeUntilStart();
            if (until_start > 0) {
                wake = std::min(wake, now + until_start);
            } else {
                pacer.Start(format.interval, now);
                delivering = true;
                first = true;
                failed = false;
            }
        }
        if (delivering && !failed) {
            int64_t when = 0;
            if (pacer.Poll(now, feed_.latest(), feed_.producer_alive(now), &when)) {
                uint64_t frame = 0;
                const HRESULT hr = Deliver(now, &format, &frame, first);
                if (hr == S_OK) {
                    pacer.Delivered(now, frame);
                    first = false;
                    continue;
                }
                if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
                    break;
                }
                // Downstream refused (S_FALSE or an error): stop until the next run.
                Log("camera %d: delivering stopped (0x%08lX)", camera_ + 1, static_cast<unsigned long>(hr));
                failed = true;
            } else {
                wake = std::min(wake, when);
            }
        }

        HANDLE handles[4] = {stop_event_, wake_event_, nullptr, nullptr};
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

int64_t OutputPin::TimeUntilStart()
{
    IReferenceClock* clock = filter_->reference_clock();
    if (!clock) {
        return 0;
    }
    REFERENCE_TIME t = 0;
    const bool ok = SUCCEEDED(clock->GetTime(&t));
    clock->Release();
    // Never more than a second: a start time far away is the graph's mistake.
    return ok ? std::clamp<int64_t>(filter_->start_time() - t, 0, shm::kTicksPerSecond) : 0;
}

HRESULT OutputPin::Deliver(int64_t now, VideoFormat* format, uint64_t* frame, bool first)
{
    IMediaSample* sample = nullptr;
    HRESULT hr = stream_allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
    if (FAILED(hr) || !sample) {
        return FAILED(hr) ? hr : E_FAIL;
    }
    // A renderer may hand back a sample with a new type: same picture, wider rows.
    AM_MEDIA_TYPE* changed = nullptr;
    if (sample->GetMediaType(&changed) == S_OK && changed) {
        VideoFormat f;
        if (ParseVideoType(*changed, &f) && SameVideo(f, *format)) {
            if (f.stride != format->stride) {
                Log("camera %d: downstream asks for rows of %d bytes", camera_ + 1, f.stride);
            }
            f.interval = format->interval;
            *format = f;
            std::lock_guard<std::mutex> lock(type_lock_);
            mt_ = MediaType(*changed);
        }
        DeleteMediaType(changed);
    }
    BYTE* data = nullptr;
    if (FAILED(sample->GetPointer(&data)) || !data || sample->GetSize() < static_cast<long>(format->image_bytes)) {
        sample->Release();
        return VFW_E_BUFFER_OVERFLOW;
    }
    *frame = feed_.Render(SpanFor(*format, data), now, format->bottom_up);

    // Stream time: the graph clock's time since Run(), or the frame count without a clock.
    REFERENCE_TIME start = static_cast<REFERENCE_TIME>(delivered_) * format->interval;
    if (IReferenceClock* clock = filter_->reference_clock()) {
        REFERENCE_TIME t = 0;
        if (SUCCEEDED(clock->GetTime(&t))) {
            start = t - filter_->start_time() + stream_offset_.load();
        }
        clock->Release();
    }
    if (!first && start <= last_start_) {
        start = last_start_ + 1;
    }
    last_start_ = start;
    REFERENCE_TIME end = start + format->interval;
    LONGLONG media_start = static_cast<LONGLONG>(delivered_), media_end = media_start + 1;
    sample->SetTime(&start, &end);
    sample->SetMediaTime(&media_start, &media_end);
    sample->SetSyncPoint(TRUE);
    sample->SetDiscontinuity(first ? TRUE : FALSE);
    sample->SetPreroll(FALSE);
    sample->SetActualDataLength(static_cast<long>(format->image_bytes));
    hr = stream_input_->Receive(sample);
    sample->Release();
    if (hr != S_OK) {
        return FAILED(hr) ? hr : S_FALSE;
    }
    delivered_++;
    feed_.CountDelivery(*frame != 0 && *frame != last_frame_sent_);
    last_frame_sent_ = *frame;
    return S_OK;
}

}  // namespace vcam::win
