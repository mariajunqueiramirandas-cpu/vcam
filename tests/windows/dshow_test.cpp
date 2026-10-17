// End-to-end test of the DirectShow capture filter: loads vcamdshow.dll
// in-process, connects its pin to an input pin implemented here (the way a
// capture app's graph would), runs it on the system clock and checks the
// samples against what a real producer, using the SDK, sends.
//
//     dshow_test [path\to\vcamdshow.dll]
#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "vcam.h"
#include "vcam/modes.h"

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAILED line %d: %s: ", __LINE__, #cond);                 \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define REQUIRE(cond, ...)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FATAL line %d: %s: ", __LINE__, #cond);                  \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

constexpr int kCamera = 4;  // test on the last slot, away from cameras people use

const GUID kSubtypeI420 = {0x30323449, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

void FreeType(AM_MEDIA_TYPE* mt)
{
    if (!mt) {
        return;
    }
    CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) {
        mt->pUnk->Release();
    }
    CoTaskMemFree(mt);
}

const VIDEOINFOHEADER* Vih(const AM_MEDIA_TYPE& mt)
{
    return reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
}

// --- an allocator that, once asked, attaches a new media type to a sample -----
// (what a video renderer does to ask for wider rows)

class TypeChangingAllocator final : public IMemAllocator {
public:
    TypeChangingAllocator()
    {
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                           reinterpret_cast<void**>(&inner_))),
                "CLSID_MemoryAllocator");
    }
    ~TypeChangingAllocator() { Free(); }

    // Releases the system allocator. Called before CoUninitialize, which may
    // unload quartz.dll: releasing it afterwards (from the destructor, at the
    // end of main) would call into unloaded code.
    void Free()
    {
        if (inner_) {
            inner_->Release();
            inner_ = nullptr;
        }
    }

    void ChangeTypeOnNextSample(const AM_MEDIA_TYPE& mt)
    {
        std::lock_guard<std::mutex> lock(lock_);
        pending_ = mt;
        pending_.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mt.cbFormat));
        memcpy(pending_.pbFormat, mt.pbFormat, mt.cbFormat);
        has_pending_ = true;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IMemAllocator) {
            *ppv = static_cast<IMemAllocator*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { return --refs_; }  // lives on the test's stack

    STDMETHODIMP SetProperties(ALLOCATOR_PROPERTIES* request, ALLOCATOR_PROPERTIES* actual) override
    {
        return inner_->SetProperties(request, actual);
    }
    STDMETHODIMP GetProperties(ALLOCATOR_PROPERTIES* props) override { return inner_->GetProperties(props); }
    STDMETHODIMP Commit() override { return inner_->Commit(); }
    STDMETHODIMP Decommit() override { return inner_->Decommit(); }
    STDMETHODIMP GetBuffer(IMediaSample** sample, REFERENCE_TIME* start, REFERENCE_TIME* end, DWORD flags) override
    {
        const HRESULT hr = inner_->GetBuffer(sample, start, end, flags);
        std::lock_guard<std::mutex> lock(lock_);
        if (SUCCEEDED(hr) && has_pending_) {
            (*sample)->SetMediaType(&pending_);
            CoTaskMemFree(pending_.pbFormat);
            has_pending_ = false;
        }
        return hr;
    }
    STDMETHODIMP ReleaseBuffer(IMediaSample* sample) override { return inner_->ReleaseBuffer(sample); }

    std::atomic<ULONG> refs_{0};

private:
    IMemAllocator* inner_ = nullptr;
    std::mutex lock_;
    AM_MEDIA_TYPE pending_{};
    bool has_pending_ = false;
};

// --- the input pin of a pretend capture application ----------------------------------

struct Received {
    std::vector<uint8_t> data;
    REFERENCE_TIME start = 0, end = 0;
    bool timed = false;
    bool discontinuity = false;
    long length = 0;
};

class SinkPin final : public IPin, public IMemInputPin {
public:
    SinkPin() { event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~SinkPin() { CloseHandle(event_); }

    // Which types ReceiveConnection takes: (subtype, width, height); none = any.
    struct Accept {
        GUID subtype;
        LONG width, height;
    };
    std::vector<Accept> accept;
    TypeChangingAllocator* allocator = nullptr;  // offered by GetAllocator when set
    long buffer_bytes = 0;                       // asked for by GetAllocatorRequirements when set

    // IUnknown (lives on the test's stack)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IPin) {
            *ppv = static_cast<IPin*>(this);
        } else if (riid == IID_IMemInputPin) {
            *ppv = static_cast<IMemInputPin*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { return --refs_; }

    // IPin, input side
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override
    {
        if (!connector || !mt) {
            return E_POINTER;
        }
        if (connector_) {
            return VFW_E_ALREADY_CONNECTED;
        }
        if (QueryAccept(mt) != S_OK) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        connector_ = connector;
        connector_->AddRef();
        std::lock_guard<std::mutex> lock(lock_);
        Copy(*mt);
        return S_OK;
    }
    STDMETHODIMP Disconnect() override
    {
        if (!connector_) {
            return S_FALSE;
        }
        connector_->Release();
        connector_ = nullptr;
        if (notified_) {
            notified_->Release();
            notified_ = nullptr;
        }
        return S_OK;
    }
    STDMETHODIMP ConnectedTo(IPin** pin) override
    {
        *pin = connector_;
        if (!connector_) {
            return VFW_E_NOT_CONNECTED;
        }
        connector_->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override
    {
        info->pFilter = nullptr;
        info->dir = PINDIR_INPUT;
        wcscpy_s(info->achName, MAX_PIN_NAME, L"In");
        return S_OK;
    }
    STDMETHODIMP QueryDirection(PIN_DIRECTION* direction) override
    {
        *direction = PINDIR_INPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override
    {
        if (mt->majortype != MEDIATYPE_Video || mt->formattype != FORMAT_VideoInfo || !mt->pbFormat) {
            return S_FALSE;
        }
        if (accept.empty()) {
            return S_OK;
        }
        const BITMAPINFOHEADER& bmi = Vih(*mt)->bmiHeader;
        for (const Accept& a : accept) {
            if (a.subtype == mt->subtype && a.width == bmi.biWidth && a.height == std::labs(bmi.biHeight)) {
                return S_OK;
            }
        }
        return S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator** out) override
    {
        if (!allocator) {
            *out = nullptr;
            return VFW_E_NO_ALLOCATOR;
        }
        *out = allocator;
        allocator->AddRef();
        return S_OK;
    }
    STDMETHODIMP NotifyAllocator(IMemAllocator* a, BOOL) override
    {
        if (notified_) {
            notified_->Release();
        }
        notified_ = a;
        notified_->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* props) override
    {
        if (!buffer_bytes) {
            return E_NOTIMPL;
        }
        memset(props, 0, sizeof(*props));
        props->cBuffers = 4;
        props->cbBuffer = buffer_bytes;
        props->cbAlign = 1;
        return S_OK;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override
    {
        Received r;
        BYTE* data = nullptr;
        sample->GetPointer(&data);
        r.length = sample->GetActualDataLength();
        if (data && r.length > 0) {
            r.data.assign(data, data + r.length);
        }
        r.timed = sample->GetTime(&r.start, &r.end) == S_OK;
        r.discontinuity = sample->IsDiscontinuity() == S_OK;
        {
            std::lock_guard<std::mutex> lock(lock_);
            last_ = std::move(r);
            starts_.push_back(last_.start);
            discontinuities_ += last_.discontinuity ? 1 : 0;
            count_++;
        }
        SetEvent(event_);
        return S_OK;
    }
    STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long count, long* processed) override
    {
        for (*processed = 0; *processed < count; (*processed)++) {
            Receive(samples[*processed]);
        }
        return S_OK;
    }
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

    // --- for the test ---
    bool connected() const { return connector_ != nullptr; }
    int count()
    {
        std::lock_guard<std::mutex> lock(lock_);
        return count_;
    }
    int discontinuities()
    {
        std::lock_guard<std::mutex> lock(lock_);
        return discontinuities_;
    }
    std::vector<REFERENCE_TIME> starts()
    {
        std::lock_guard<std::mutex> lock(lock_);
        return starts_;
    }
    void Reset()
    {
        std::lock_guard<std::mutex> lock(lock_);
        count_ = 0;
        discontinuities_ = 0;
        starts_.clear();
    }
    // Waits until `n` more samples than now have arrived; returns the newest.
    Received Wait(int n = 1, DWORD timeout_ms = 2000)
    {
        const int target = count() + n;
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        while (count() < target && GetTickCount64() < deadline) {
            WaitForSingleObject(event_, 50);
        }
        std::lock_guard<std::mutex> lock(lock_);
        return last_;
    }
    VIDEOINFOHEADER format()
    {
        std::lock_guard<std::mutex> lock(lock_);
        return vih_;
    }
    GUID subtype()
    {
        std::lock_guard<std::mutex> lock(lock_);
        return subtype_;
    }

private:
    void Copy(const AM_MEDIA_TYPE& mt)
    {
        subtype_ = mt.subtype;
        memcpy(&vih_, mt.pbFormat, sizeof(vih_));
    }

    std::atomic<ULONG> refs_{0};
    IPin* connector_ = nullptr;
    IMemAllocator* notified_ = nullptr;
    std::mutex lock_;
    GUID subtype_ = GUID_NULL;
    VIDEOINFOHEADER vih_{};
    Received last_;
    std::vector<REFERENCE_TIME> starts_;
    int count_ = 0;
    int discontinuities_ = 0;
    HANDLE event_ = nullptr;
};

// --- a graph that only knows how to reconnect ----------------------------------------

class ReconnectingGraph final : public IFilterGraph {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IFilterGraph) {
            *ppv = static_cast<IFilterGraph*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { return --refs_; }

    STDMETHODIMP AddFilter(IBaseFilter*, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP RemoveFilter(IBaseFilter*) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFilters(IEnumFilters**) override { return E_NOTIMPL; }
    STDMETHODIMP FindFilterByName(LPCWSTR, IBaseFilter**) override { return E_NOTIMPL; }
    STDMETHODIMP ConnectDirect(IPin*, IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP Reconnect(IPin* pin) override
    {
        reconnects++;
        IPin* other = nullptr;
        HRESULT hr = pin->ConnectedTo(&other);
        if (FAILED(hr)) {
            return hr;
        }
        pin->Disconnect();
        other->Disconnect();
        hr = pin->Connect(other, nullptr);
        other->Release();
        return hr;
    }
    STDMETHODIMP Disconnect(IPin* pin) override { return pin->Disconnect(); }
    STDMETHODIMP SetDefaultSyncSource() override { return E_NOTIMPL; }

    int reconnects = 0;

private:
    std::atomic<ULONG> refs_{0};
};

// --- helpers ------------------------------------------------------------------------------

struct Pixel {
    int a = -1, b = -1, c = -1;  // Y, U, V or R, G, B
};

// Reads pixel (x, y) of a received frame, given the connection's format.
Pixel At(const Received& r, const GUID& subtype, const VIDEOINFOHEADER& vih, int x, int y)
{
    Pixel p;
    const BITMAPINFOHEADER& bmi = vih.bmiHeader;
    const int stride_px = static_cast<int>(bmi.biWidth);
    const int rows = static_cast<int>(std::labs(bmi.biHeight));
    const uint8_t* d = r.data.data();
    if (r.data.empty()) {
        return p;
    }
    if (subtype == MEDIASUBTYPE_NV12 || subtype == kSubtypeI420) {
        p.a = d[static_cast<size_t>(y) * stride_px + x];
        const uint8_t* chroma = d + static_cast<size_t>(stride_px) * rows;
        if (subtype == MEDIASUBTYPE_NV12) {
            p.b = chroma[static_cast<size_t>(y / 2) * stride_px + (x / 2) * 2];
            p.c = chroma[static_cast<size_t>(y / 2) * stride_px + (x / 2) * 2 + 1];
        } else {
            const int half = stride_px / 2;
            p.b = chroma[static_cast<size_t>(y / 2) * half + x / 2];
            p.c = chroma[static_cast<size_t>(half) * (rows / 2) + static_cast<size_t>(y / 2) * half + x / 2];
        }
    } else if (subtype == MEDIASUBTYPE_YUY2) {
        const uint8_t* row = d + static_cast<size_t>(y) * stride_px * 2 + (x / 2) * 4;
        p.a = row[(x & 1) ? 2 : 0];
        p.b = row[1];
        p.c = row[3];
    } else {  // RGB32, bottom-up when the height is positive
        const int memory_row = bmi.biHeight > 0 ? rows - 1 - y : y;
        const uint8_t* px = d + static_cast<size_t>(memory_row) * stride_px * 4 + static_cast<size_t>(x) * 4;
        p.a = px[2];
        p.b = px[1];
        p.c = px[0];
    }
    return p;
}

bool Near(int a, int b, int tolerance = 3)
{
    return std::abs(a - b) <= tolerance;
}

bool IsRedYuv(const Pixel& p)
{
    return Near(p.a, 81) && Near(p.b, 90) && Near(p.c, 240);
}

void SendSolid(vcam_producer* producer, int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.size(); i += 4) {
        frame[i] = b, frame[i + 1] = g, frame[i + 2] = r, frame[i + 3] = 255;
    }
    const int rc = vcam_send_packed(producer, VCAM_FORMAT_BGRA, width, height, frame.data(), 0);
    CHECK(rc == VCAM_OK, "vcam_send_packed: %s", vcam_error_string(rc));
}

// Top half red, bottom half blue.
void SendHalves(vcam_producer* producer, int width, int height)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t* px = &frame[(static_cast<size_t>(y) * width + x) * 4];
            const bool top = y < height / 2;
            px[0] = top ? 0 : 255, px[1] = 0, px[2] = top ? 255 : 0, px[3] = 255;
        }
    }
    vcam_send_packed(producer, VCAM_FORMAT_BGRA, width, height, frame.data(), 0);
}

// The capability matching subtype, size and rate.
AM_MEDIA_TYPE* FindCaps(IAMStreamConfig* config, const GUID& subtype, LONG width, LONG height, int fps)
{
    int count = 0, size = 0;
    config->GetNumberOfCapabilities(&count, &size);
    std::vector<BYTE> caps(static_cast<size_t>(size));
    for (int i = 0; i < count; i++) {
        AM_MEDIA_TYPE* mt = nullptr;
        if (config->GetStreamCaps(i, &mt, caps.data()) != S_OK) {
            continue;
        }
        const VIDEOINFOHEADER* vih = Vih(*mt);
        if (mt->subtype == subtype && vih->bmiHeader.biWidth == width && std::labs(vih->bmiHeader.biHeight) == height &&
            vih->AvgTimePerFrame == 10'000'000 / fps) {
            return mt;
        }
        FreeType(mt);
    }
    return nullptr;
}

// A filter instance with its pin, connected to a sink, on the system clock.
class Capture {
public:
    Capture(IClassFactory* factory, IReferenceClock* clock) : clock_(clock)
    {
        REQUIRE(SUCCEEDED(factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&filter_))),
                "CreateInstance");
        REQUIRE(SUCCEEDED(filter_->FindPin(L"Capture", &pin_)), "FindPin");
        REQUIRE(SUCCEEDED(pin_->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&config_))),
                "IAMStreamConfig");
        if (clock_) {
            filter_->SetSyncSource(clock_);
        }
    }
    ~Capture()
    {
        filter_->Stop();
        if (sink_) {
            pin_->Disconnect();
            sink_->Disconnect();
        }
        config_->Release();
        pin_->Release();
        filter_->Release();
    }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Connects with the capability (subtype, size, fps), or with whatever the
    // pin proposes first that the sink accepts when `exact` is false.
    void Connect(SinkPin* sink, const GUID& subtype, LONG width, LONG height, int fps, bool exact = true)
    {
        sink_ = sink;
        sink->accept = {{subtype, width, height}};
        if (exact) {
            AM_MEDIA_TYPE* mt = FindCaps(config_, subtype, width, height, fps);
            REQUIRE(mt, "no capability %ldx%ld@%d", width, height, fps);
            const HRESULT hr = pin_->Connect(sink, mt);
            FreeType(mt);
            REQUIRE(SUCCEEDED(hr), "Connect (0x%08lX)", static_cast<unsigned long>(hr));
        } else {
            const HRESULT hr = pin_->Connect(sink, nullptr);
            REQUIRE(SUCCEEDED(hr), "Connect without a type (0x%08lX)", static_cast<unsigned long>(hr));
        }
        REQUIRE(sink->connected(), "sink connected");
    }

    void Run()
    {
        REFERENCE_TIME now = 0;
        if (clock_) {
            clock_->GetTime(&now);
        }
        REQUIRE(SUCCEEDED(filter_->Pause()), "Pause");
        // Like a graph manager: the stream starts a little in the future.
        REQUIRE(SUCCEEDED(filter_->Run(now + 1'000'000)), "Run");
    }
    void Stop() { CHECK(SUCCEEDED(filter_->Stop()), "Stop"); }

    IBaseFilter* filter() const { return filter_; }
    IPin* pin() const { return pin_; }
    IAMStreamConfig* config() const { return config_; }

private:
    IReferenceClock* clock_;
    IBaseFilter* filter_ = nullptr;
    IPin* pin_ = nullptr;
    IAMStreamConfig* config_ = nullptr;
    SinkPin* sink_ = nullptr;
};

bool WaitForTarget(vcam_producer* producer, int width, int height, int format, DWORD timeout_ms = 2000)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    vcam_status status{};
    status.struct_size = sizeof(status);
    while (GetTickCount64() < deadline) {
        vcam_get_status(producer, &status);
        if (status.consumer_active && status.width == width && status.height == height && status.format == format) {
            return true;
        }
        Sleep(20);
    }
    printf("  producer sees %dx%d format %d (active %d)\n", status.width, status.height, status.format,
           status.consumer_active);
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dll_path = argc > 1 ? argv[1] : "vcamdshow.dll";
    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "CoInitializeEx");

    HMODULE dll = LoadLibraryA(dll_path.c_str());
    REQUIRE(dll, "LoadLibrary(%s) failed: %lu", dll_path.c_str(), GetLastError());
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    using CanUnloadFn = HRESULT(STDAPICALLTYPE*)();
    auto get_class_object =
        reinterpret_cast<GetClassObjectFn>(reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    auto can_unload = reinterpret_cast<CanUnloadFn>(reinterpret_cast<void*>(GetProcAddress(dll, "DllCanUnloadNow")));
    REQUIRE(get_class_object && can_unload, "exports missing");
    REQUIRE(GetProcAddress(dll, "DllRegisterServer") && GetProcAddress(dll, "DllUnregisterServer"),
            "registration exports missing");

    IReferenceClock* clock = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemClock, nullptr, CLSCTX_INPROC_SERVER, IID_IReferenceClock,
                                reinterpret_cast<void**>(&clock)))) {
        printf("  (no system clock: timestamps from the frame count)\n");
    }

    // Declared before the filters, so they outlive every connection.
    SinkPin sink, second_sink;
    TypeChangingAllocator allocator;
    ReconnectingGraph graph;
    {
        printf("[filter and pin]\n");
        IClassFactory* factory = nullptr;
        REQUIRE(SUCCEEDED(get_class_object(vcam::win::DirectShowClsid(kCamera - 1), IID_IClassFactory,
                                           reinterpret_cast<void**>(&factory))),
                "DllGetClassObject");
        IClassFactory* none = nullptr;
        CHECK(get_class_object(vcam::win::SourceClsid(0), IID_IClassFactory, reinterpret_cast<void**>(&none)) ==
                  CLASS_E_CLASSNOTAVAILABLE,
              "the media source's CLSID is not ours");

        Capture a(factory, clock);
        CLSID clsid = GUID_NULL;
        CHECK(SUCCEEDED(a.filter()->GetClassID(&clsid)) && clsid == vcam::win::DirectShowClsid(kCamera - 1),
              "GetClassID");
        IAMFilterMiscFlags* flags = nullptr;
        CHECK(SUCCEEDED(a.filter()->QueryInterface(IID_IAMFilterMiscFlags, reinterpret_cast<void**>(&flags))) &&
                  flags->GetMiscFlags() == AM_FILTER_MISC_FLAGS_IS_SOURCE,
              "a source filter");
        if (flags) {
            flags->Release();
        }
        IEnumPins* pins = nullptr;
        REQUIRE(SUCCEEDED(a.filter()->EnumPins(&pins)), "EnumPins");
        IPin* listed[2] = {};
        ULONG fetched = 0;
        CHECK(pins->Next(2, listed, &fetched) == S_FALSE && fetched == 1 && listed[0] == a.pin(), "one pin");
        for (ULONG i = 0; i < fetched; i++) {
            listed[i]->Release();
        }
        pins->Release();
        PIN_DIRECTION direction = PINDIR_INPUT;
        CHECK(SUCCEEDED(a.pin()->QueryDirection(&direction)) && direction == PINDIR_OUTPUT, "output pin");
        PIN_INFO info{};
        CHECK(SUCCEEDED(a.pin()->QueryPinInfo(&info)) && info.pFilter == a.filter() && wcscmp(info.achName, L"Capture") == 0,
              "pin info");
        if (info.pFilter) {
            info.pFilter->Release();
        }
        IPin* missing = nullptr;
        CHECK(a.filter()->FindPin(L"Preview", &missing) == VFW_E_NOT_FOUND, "no other pin");

        IKsPropertySet* props = nullptr;
        REQUIRE(SUCCEEDED(a.pin()->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&props))),
                "IKsPropertySet");
        GUID category = GUID_NULL;
        DWORD returned = 0;
        CHECK(SUCCEEDED(props->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category),
                                   &returned)) &&
                  category == PIN_CATEGORY_CAPTURE && returned == sizeof(GUID),
              "capture pin category");
        DWORD support = 0;
        CHECK(SUCCEEDED(props->QuerySupported(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, &support)) && support == 1,
              "category is readable");
        CHECK(FAILED(props->Set(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category))),
              "category is read-only");
        props->Release();

        printf("[capabilities]\n");
        const vcam::win::Config config = vcam::win::LoadConfig();
        const auto modes = vcam::StandardModes(config.max_width, config.max_height, config.allow_60fps);
        int count = 0, size = 0;
        CHECK(SUCCEEDED(a.config()->GetNumberOfCapabilities(&count, &size)) &&
                  count == static_cast<int>(modes.size()) * 4 && size == static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS)),
              "%d capabilities of %d bytes", count, size);
        VIDEO_STREAM_CONFIG_CAPS caps{};
        AM_MEDIA_TYPE* mt = nullptr;
        REQUIRE(a.config()->GetStreamCaps(0, &mt, reinterpret_cast<BYTE*>(&caps)) == S_OK, "GetStreamCaps(0)");
        CHECK(mt->subtype == MEDIASUBTYPE_NV12 && Vih(*mt)->bmiHeader.biWidth == static_cast<LONG>(modes[0].width) &&
                  Vih(*mt)->bmiHeader.biHeight == static_cast<LONG>(modes[0].height) &&
                  caps.MinFrameInterval == static_cast<REFERENCE_TIME>(10'000'000 / modes[0].fps) &&
                  caps.MaxOutputSize.cx == Vih(*mt)->bmiHeader.biWidth,
              "first capability is the default mode in NV12");
        FreeType(mt);
        REQUIRE(a.config()->GetStreamCaps(3, &mt, reinterpret_cast<BYTE*>(&caps)) == S_OK, "GetStreamCaps(3)");
        CHECK(mt->subtype == MEDIASUBTYPE_RGB32 && Vih(*mt)->bmiHeader.biHeight > 0, "RGB32 is bottom-up");
        FreeType(mt);
        CHECK(a.config()->GetStreamCaps(count, &mt, reinterpret_cast<BYTE*>(&caps)) == S_FALSE, "past the end");
        CHECK(SUCCEEDED(a.config()->GetFormat(&mt)) && mt->subtype == MEDIASUBTYPE_NV12, "default format");
        FreeType(mt);
        IEnumMediaTypes* types = nullptr;
        ULONG enumerated = 0;
        if (SUCCEEDED(a.pin()->EnumMediaTypes(&types))) {
            AM_MEDIA_TYPE* t = nullptr;
            while (types->Next(1, &t, &fetched) == S_OK) {
                enumerated++;
                FreeType(t);
            }
            types->Release();
        }
        CHECK(enumerated == static_cast<ULONG>(count), "EnumMediaTypes lists every capability (%lu)", enumerated);

        printf("[placeholder without a producer]\n");
        a.Connect(&sink, MEDIASUBTYPE_NV12, 640, 480, 30, false);  // the pin finds a type the sink takes
        AM_MEDIA_TYPE connection{};
        CHECK(SUCCEEDED(a.pin()->ConnectionMediaType(&connection)) && connection.subtype == MEDIASUBTYPE_NV12 &&
                  Vih(connection)->AvgTimePerFrame == 333333,
              "connected with NV12 640x480 at 30 fps");
        CoTaskMemFree(connection.pbFormat);
        CHECK(a.pin()->Connect(&sink, nullptr) == VFW_E_ALREADY_CONNECTED, "already connected");
        a.Run();
        Received r = sink.Wait(3);
        REQUIRE(!r.data.empty(), "no sample");
        CHECK(r.length == 640 * 480 * 3 / 2, "sample length %ld", r.length);
        Pixel corner = At(r, sink.subtype(), sink.format(), 2, 2);
        CHECK(corner.a >= 25 && corner.a <= 60, "placeholder background, Y=%d", corner.a);
        FILTER_STATE state = State_Stopped;
        CHECK(a.filter()->GetState(0, &state) == S_OK && state == State_Running, "running");
        CHECK(sink.discontinuities() == 1, "only the first sample is a discontinuity (%d)", sink.discontinuities());
        if (clock) {
            const REFERENCE_TIME first_start = sink.starts().front();
            CHECK(first_start >= 0 && first_start < 333333, "stream time starts at zero (%lld)",
                  static_cast<long long>(first_start));
        }

        printf("[producer frames]\n");
        vcam_options options = VCAM_OPTIONS_INIT;
        options.camera = kCamera;
        vcam_producer* producer = nullptr;
        int rc = vcam_open(&options, &producer);
        REQUIRE(rc == VCAM_OK, "vcam_open: %s", vcam_error_string(rc));
        CHECK(WaitForTarget(producer, 640, 480, VCAM_FORMAT_NV12), "the producer learns the format");
        SendSolid(producer, 1280, 720, 255, 0, 0);  // 16:9 into 4:3: letterboxed
        Sleep(100);
        r = sink.Wait(2);
        Pixel centre = At(r, sink.subtype(), sink.format(), 320, 240);
        Pixel top = At(r, sink.subtype(), sink.format(), 320, 10);
        CHECK(IsRedYuv(centre), "red centre %d %d %d", centre.a, centre.b, centre.c);
        CHECK(Near(top.a, 16) && Near(top.b, 128) && Near(top.c, 128), "black bar %d %d %d", top.a, top.b, top.c);

        printf("[pacing]\n");
        sink.Reset();
        const ULONGLONG wall_start = GetTickCount64();
        int fresh = 0;
        for (int i = 0; i < 30; i++) {
            SendSolid(producer, 640, 480, 0, static_cast<uint8_t>(100 + i * 5), 0);
            r = sink.Wait(1);
            if (At(r, sink.subtype(), sink.format(), 100, 100).c < 110) {  // green, not the red before
                fresh++;
            }
        }
        const ULONGLONG wall = GetTickCount64() - wall_start;
        const std::vector<REFERENCE_TIME> starts = sink.starts();
        CHECK(starts.size() >= 29, "%zu samples", starts.size());
        if (starts.size() >= 2) {
            const double seconds = static_cast<double>(starts.back() - starts.front()) / 1e7;
            const double expected = static_cast<double>(starts.size() - 1) / 30.0;
            CHECK(seconds > expected * 0.85 && seconds < expected * 1.3, "%zu intervals took %.3f s",
                  starts.size() - 1, seconds);
            bool increasing = true;
            for (size_t i = 1; i < starts.size(); i++) {
                increasing = increasing && starts[i] > starts[i - 1];
            }
            CHECK(increasing, "timestamps increase");
        }
        CHECK(wall < 2000, "30 frames took %llu ms of wall time", static_cast<unsigned long long>(wall));
        CHECK(fresh >= 29, "%d of 30 samples showed the producer's frame", fresh);
        vcam_status status{};
        status.struct_size = sizeof(status);
        vcam_get_status(producer, &status);
        CHECK(status.frames_delivered >= 25, "frames_delivered %llu",
              static_cast<unsigned long long>(status.frames_delivered));

        printf("[a second application with another format]\n");
        Capture b(factory, clock);
        b.Connect(&second_sink, MEDIASUBTYPE_YUY2, 1280, 720, 30);
        b.Run();
        SendSolid(producer, 1280, 720, 255, 0, 0);
        Sleep(150);
        // The first application keeps the format; the second rescales the picture, not its bars.
        CHECK(WaitForTarget(producer, 640, 480, VCAM_FORMAT_NV12, 500), "the first consumer keeps the request");
        r = second_sink.Wait(2);
        Pixel b_corner = At(r, second_sink.subtype(), second_sink.format(), 4, 4);
        CHECK(IsRedYuv(b_corner), "second app: red edge to edge, %d %d %d", b_corner.a, b_corner.b, b_corner.c);

        printf("[hand-over when the first application stops]\n");
        a.Stop();
        CHECK(WaitForTarget(producer, 1280, 720, VCAM_FORMAT_YUY2), "the second consumer takes over the request");
        SendSolid(producer, 1280, 720, 255, 0, 0);
        Sleep(100);
        r = second_sink.Wait(2);
        b_corner = At(r, second_sink.subtype(), second_sink.format(), 4, 4);
        CHECK(IsRedYuv(b_corner), "second app after the hand-over: %d %d %d", b_corner.a, b_corner.b, b_corner.c);
        CHECK(r.length == 1280 * 720 * 2, "YUY2 sample length %ld", r.length);

        printf("[stop and restart]\n");
        b.Stop();
        const int before = second_sink.count();
        Sleep(200);
        CHECK(second_sink.count() == before, "no samples while stopped");
        second_sink.Reset();
        b.Run();
        r = second_sink.Wait(3);
        CHECK(!r.data.empty() && second_sink.discontinuities() == 1, "restarted with one discontinuity (%d)",
              second_sink.discontinuities());
        b.Stop();

        printf("[SetFormat and reconnect: I420 1280x720]\n");
        CHECK(SUCCEEDED(a.filter()->JoinFilterGraph(&graph, L"vcam test")), "JoinFilterGraph");
        FILTER_INFO filter_info{};
        CHECK(SUCCEEDED(a.filter()->QueryFilterInfo(&filter_info)) && filter_info.pGraph == &graph &&
                  wcscmp(filter_info.achName, L"vcam test") == 0,
              "filter info");
        if (filter_info.pGraph) {
            filter_info.pGraph->Release();
        }
        sink.accept.push_back({kSubtypeI420, 1280, 720});
        mt = FindCaps(a.config(), kSubtypeI420, 1280, 720, 30);
        REQUIRE(mt, "I420 capability");
        HRESULT hr = a.config()->SetFormat(mt);
        CHECK(SUCCEEDED(hr) && graph.reconnects == 1, "SetFormat reconnects (0x%08lX, %d)",
              static_cast<unsigned long>(hr), graph.reconnects);
        FreeType(mt);
        CHECK(sink.subtype() == kSubtypeI420 && sink.format().bmiHeader.biWidth == 1280, "reconnected with I420");
        CHECK(SUCCEEDED(a.config()->GetFormat(&mt)) && mt->subtype == kSubtypeI420, "GetFormat follows");
        FreeType(mt);
        IEnumMediaTypes* preferred = nullptr;
        enumerated = 0;
        if (SUCCEEDED(a.pin()->EnumMediaTypes(&preferred))) {
            AM_MEDIA_TYPE* t = nullptr;
            while (preferred->Next(1, &t, &fetched) == S_OK) {
                enumerated++;
                FreeType(t);
            }
            preferred->Release();
        }
        CHECK(enumerated == 1, "after SetFormat only that format is offered (%lu)", enumerated);
        a.Run();
        CHECK(WaitForTarget(producer, 1280, 720, VCAM_FORMAT_I420), "the producer follows the new format");
        SendSolid(producer, 1280, 720, 255, 0, 0);
        Sleep(100);
        r = sink.Wait(2);
        Pixel i420 = At(r, sink.subtype(), sink.format(), 640, 360);
        CHECK(IsRedYuv(i420), "I420 red %d %d %d", i420.a, i420.b, i420.c);
        CHECK(r.length == 1280 * 720 * 3 / 2, "I420 sample length %ld", r.length);
        a.Stop();
        CHECK(SUCCEEDED(a.config()->SetFormat(nullptr)), "SetFormat(nullptr) forgets the preference");
        a.pin()->Disconnect();
        sink.Disconnect();
        a.filter()->JoinFilterGraph(nullptr, nullptr);

        printf("[RGB32 is bottom-up, and a renderer's wider rows]\n");
        sink.allocator = &allocator;
        sink.buffer_bytes = 704 * 4 * 360;
        a.Connect(&sink, MEDIASUBTYPE_RGB32, 640, 360, 30);
        a.Run();
        CHECK(WaitForTarget(producer, 640, 360, VCAM_FORMAT_BGRA), "the producer renders RGB32");
        SendHalves(producer, 640, 360);
        Sleep(100);
        r = sink.Wait(2);
        Pixel up = At(r, sink.subtype(), sink.format(), 320, 10);
        Pixel down = At(r, sink.subtype(), sink.format(), 320, 350);
        CHECK(up.a == 255 && up.b == 0 && up.c == 0, "top is red: %d %d %d", up.a, up.b, up.c);
        CHECK(down.a == 0 && down.b == 0 && down.c == 255, "bottom is blue: %d %d %d", down.a, down.b, down.c);
        CHECK(r.data.size() >= 4 && r.data[0] == 255 && r.data[2] == 0, "memory starts with the bottom row");

        // The renderer asks for 704-pixel rows, as video renderers do for alignment.
        AM_MEDIA_TYPE wider{};
        mt = FindCaps(a.config(), MEDIASUBTYPE_RGB32, 640, 360, 30);
        REQUIRE(mt, "RGB32 capability");
        wider = *mt;
        VIDEOINFOHEADER vih = *Vih(*mt);
        vih.bmiHeader.biWidth = 704;
        vih.bmiHeader.biSizeImage = 704 * 4 * 360;
        vih.rcTarget = RECT{0, 0, 640, 360};
        vih.rcSource = RECT{0, 0, 640, 360};
        wider.pbFormat = reinterpret_cast<BYTE*>(&vih);
        wider.lSampleSize = vih.bmiHeader.biSizeImage;
        CHECK(a.pin()->QueryAccept(&wider) == S_OK, "the pin accepts wider rows");
        allocator.ChangeTypeOnNextSample(wider);
        FreeType(mt);
        sink.Wait(3);
        SendHalves(producer, 640, 360);
        Sleep(100);
        r = sink.Wait(2);
        VIDEOINFOHEADER padded = vih;
        up = At(r, sink.subtype(), padded, 320, 10);
        down = At(r, sink.subtype(), padded, 320, 350);
        CHECK(r.length == 704 * 4 * 360, "padded sample length %ld", r.length);
        CHECK(up.a == 255 && up.c == 0 && down.a == 0 && down.c == 255, "padded rows: top %d %d %d, bottom %d %d %d",
              up.a, up.b, up.c, down.a, down.b, down.c);
        AM_MEDIA_TYPE now_type{};
        CHECK(SUCCEEDED(a.pin()->ConnectionMediaType(&now_type)) && Vih(now_type)->bmiHeader.biWidth == 704,
              "the connection type follows the renderer");
        CoTaskMemFree(now_type.pbFormat);
        a.Stop();

        printf("[producer gone -> placeholder]\n");
        vcam_close(producer);
        a.pin()->Disconnect();
        sink.Disconnect();
        sink.allocator = nullptr;
        sink.buffer_bytes = 0;
        a.Connect(&sink, MEDIASUBTYPE_NV12, 640, 480, 30);
        a.Run();
        r = sink.Wait(3);
        corner = At(r, sink.subtype(), sink.format(), 2, 2);
        CHECK(corner.a >= 25 && corner.a <= 60, "placeholder again, Y=%d", corner.a);

        printf("[latency and teardown]\n");
        IAMPushSource* push = nullptr;
        REFERENCE_TIME latency = 0;
        CHECK(SUCCEEDED(a.pin()->QueryInterface(IID_IAMPushSource, reinterpret_cast<void**>(&push))) &&
                  SUCCEEDED(push->GetLatency(&latency)) && latency == 333333,
              "one frame of latency");
        if (push) {
            push->Release();
        }
        CHECK(a.pin()->Disconnect() == VFW_E_NOT_STOPPED, "no disconnecting while running");
        a.Stop();
        CHECK(a.filter()->GetState(0, &state) == S_OK && state == State_Stopped, "stopped");
        factory->Release();
    }
    CHECK(sink.Disconnect() == S_FALSE && second_sink.Disconnect() == S_FALSE, "the filters disconnected");
    CHECK(can_unload() == S_OK, "DllCanUnloadNow after everything is released");
    if (clock) {
        clock->Release();
    }
    allocator.Free();

    CoUninitialize();
    printf(g_failures ? "%d FAILURES\n" : "all DirectShow tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
