// The DirectShow capture filter that stands for the vcam cameras on Windows 10
// (vcamdshow.dll, one COM class per camera slot).
//
// Windows 10 has no virtual camera API, so each application loads this filter
// itself, the way it loads a webcam's: it is registered in the video capture
// category and DirectShow apps (Zoom, Teams, browsers, OBS...) list it. Every
// filter instance is a consumer of the camera's shared-memory ring (FrameFeed);
// several applications can use a camera at once.
//
// Object graph: the class factory makes a Filter, which owns its one output pin
// (the pin's reference count is the filter's). While the filter runs, a worker
// thread fills samples from the downstream allocator at the connection's frame
// rate and pushes them to the connected input pin.
#pragma once

#include "common.h"
#include "feed.h"

#include <dshow.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vcam/modes.h"

namespace vcam::win {

// Live COM objects and server locks, for DllCanUnloadNow.
extern std::atomic<long> g_objects;

// --- media types ---------------------------------------------------------------------

// Frees what an AM_MEDIA_TYPE points to (format block, pUnk), not the struct.
void FreeMediaType(AM_MEDIA_TYPE& mt);
// Frees a media type allocated with CoTaskMemAlloc, as callers get them from us.
void DeleteMediaType(AM_MEDIA_TYPE* mt);
HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE& src);

// Owns an AM_MEDIA_TYPE and its format block.
class MediaType {
public:
    MediaType();
    explicit MediaType(const AM_MEDIA_TYPE& mt);
    MediaType(const MediaType& other);
    MediaType& operator=(const MediaType& other);
    ~MediaType();

    const AM_MEDIA_TYPE& get() const { return mt_; }
    bool empty() const { return mt_.majortype == GUID_NULL; }
    // A copy for a caller, allocated with CoTaskMemAlloc (nullptr when out of memory).
    AM_MEDIA_TYPE* CreateCopy() const;

private:
    AM_MEDIA_TYPE mt_;
};

// What a media type means for writing frames into a sample.
struct VideoFormat {
    PixelFormat format = PixelFormat::Unknown;
    GUID subtype = GUID_NULL;
    int width = 0, height = 0;  // the picture
    int stride = 0;             // bytes per row of the first plane (wider than the picture when padded)
    bool bottom_up = false;     // RGB with a positive height: the last row comes first
    int64_t interval = 0;       // 100 ns per frame
    uint32_t image_bytes = 0;   // bytes one frame needs
};

// Parses a FORMAT_VideoInfo type in one of the formats the pin produces.
bool ParseVideoType(const AM_MEDIA_TYPE& mt, VideoFormat* format);
// Same pixel format and picture size (the stride may differ).
bool SameVideo(const VideoFormat& a, const VideoFormat& b);
MediaType MakeVideoType(PixelFormat format, uint32_t width, uint32_t height, uint32_t fps);

// What the pin offers: every mode in NV12, YUY2, I420 and RGB32, mode by mode.
struct OfferedType {
    VideoMode mode;
    PixelFormat format = PixelFormat::Unknown;
};
std::vector<OfferedType> OfferedTypes(const Config& config);

// --- the filter --------------------------------------------------------------------------

class Filter;

class OutputPin final : public IPin, public IAMStreamConfig, public IKsPropertySet, public IAMPushSource {
public:
    OutputPin(Filter* filter, int camera);
    ~OutputPin();
    OutputPin(const OutputPin&) = delete;
    OutputPin& operator=(const OutputPin&) = delete;

    // IUnknown (the reference count is the filter's)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* receive, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* direction) override;
    STDMETHODIMP QueryId(LPWSTR* id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** types) override;
    STDMETHODIMP QueryInternalConnections(IPin** pins, ULONG* count) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** pmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** pmt, BYTE* caps) override;

    // IKsPropertySet: the pin category (capture), which apps look for.
    STDMETHODIMP Set(REFGUID set, DWORD id, LPVOID instance, DWORD instance_bytes, LPVOID data,
                     DWORD data_bytes) override;
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID instance, DWORD instance_bytes, LPVOID data, DWORD data_bytes,
                     DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    // IAMLatency / IAMPushSource
    STDMETHODIMP GetLatency(REFERENCE_TIME* latency) override;
    STDMETHODIMP GetPushSourceFlags(ULONG* flags) override;
    STDMETHODIMP SetPushSourceFlags(ULONG flags) override;
    STDMETHODIMP SetStreamOffset(REFERENCE_TIME offset) override;
    STDMETHODIMP GetStreamOffset(REFERENCE_TIME* offset) override;
    STDMETHODIMP GetMaxStreamOffset(REFERENCE_TIME* offset) override;
    STDMETHODIMP SetMaxStreamOffset(REFERENCE_TIME offset) override;

    // Called by the filter, with its lock held.
    HRESULT Active();    // stopped -> paused: commit the allocator, start the worker
    void Inactive();     // -> stopped: stop the worker, decommit
    void Wake();         // the filter's state changed
    void ReleaseConnection();

private:
    HRESULT TryConnect(IPin* receive, const AM_MEDIA_TYPE& mt);
    HRESULT DecideAllocator(IPin* receive);
    bool Accepts(const AM_MEDIA_TYPE& mt) const;
    std::vector<MediaType> Candidates() const;
    MediaType DefaultType() const;

    void Worker();
    int64_t TimeUntilStart();  // 100 ns until the graph's start time, 0 once reached or without a clock
    HRESULT Deliver(int64_t now, VideoFormat* format, uint64_t* frame, bool first);

    Filter* filter_;
    int camera_;
    Config config_;
    std::vector<OfferedType> offered_;

    // The connection; changed on the graph's thread with the filter lock held.
    IMemInputPin* input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    MediaType preferred_;  // from IAMStreamConfig::SetFormat
    // What downstream may ask about while we stream (with the filter lock
    // possibly held by a Stop waiting for us) is also behind type_lock_. The
    // worker updates mt_ when a renderer changes the stride.
    mutable std::mutex type_lock_;
    IPin* connected_ = nullptr;
    VideoFormat format_;
    MediaType mt_;

    // Streaming.
    std::thread worker_;
    HANDLE stop_event_ = nullptr;
    HANDLE wake_event_ = nullptr;
    FrameFeed feed_;
    IMemInputPin* stream_input_ = nullptr;
    IMemAllocator* stream_allocator_ = nullptr;
    VideoFormat stream_format_;
    uint64_t delivered_ = 0;
    uint64_t last_frame_sent_ = 0;
    REFERENCE_TIME last_start_ = 0;
    std::atomic<int64_t> stream_offset_{0};
    std::atomic<int64_t> max_stream_offset_{0};
};

class Filter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
    explicit Filter(int camera);
    Filter(const Filter&) = delete;
    Filter& operator=(const Filter&) = delete;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* clsid) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME start) override;
    STDMETHODIMP GetState(DWORD timeout_ms, FILTER_STATE* state) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** pins) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* info) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

    // For the pin.
    std::recursive_mutex& state_lock() { return lock_; }
    bool stopped() const { return state_.load() == State_Stopped; }
    bool running() const { return state_.load() == State_Running; }
    REFERENCE_TIME start_time() const { return start_.load(); }
    IReferenceClock* reference_clock();  // AddRef'd, may be null
    IFilterGraph* filter_graph() const { return graph_; }

private:
    ~Filter();

    std::atomic<ULONG> refs_{1};
    int camera_;
    // The graph's thread: state changes, connections, format changes. The
    // streaming thread never takes it.
    std::recursive_mutex lock_;
    std::atomic<FILTER_STATE> state_{State_Stopped};
    std::atomic<REFERENCE_TIME> start_{0};
    std::mutex clock_lock_;
    IReferenceClock* clock_ = nullptr;
    IFilterGraph* graph_ = nullptr;  // not referenced: the graph owns its filters
    std::wstring name_;
    OutputPin pin_;
};

}  // namespace vcam::win
