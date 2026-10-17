// The Media Foundation media source that Windows Frame Server loads for each
// vcam camera (one COM class per camera slot).
//
// Object graph: the class factory makes an Activate; Frame Server fills its
// attribute store and calls ActivateObject, which creates the MediaSource. The
// source owns one MediaStream (video capture). While the stream runs, a worker
// thread pulls frames from the shared-memory ring (FrameFeed) and answers the
// pipeline's sample requests at the negotiated frame rate.
#pragma once

#include "common.h"

#include <objbase.h>
#include <initguid.h>  // define the GUIDs used below in this module instead of linking mfuuid.lib
#include <winternl.h>  // NTSTATUS for ks.h
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <ksproxy.h>  // IKsControl (ks.h only declares it for kernel mode)
#include <ksmedia.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vcam/convert.h"
#include "vcam/modes.h"
#include "vcam/pacer.h"

namespace vcam::win {

using Microsoft::WRL::ComPtr;

// Live COM objects and server locks, for DllCanUnloadNow.
extern std::atomic<long> g_objects;

class MediaSource;

// Consumer side of the shared-memory ring for one camera.
class FrameFeed {
public:
    explicit FrameFeed(int camera);

    // Sets the format the stream delivers and renders the placeholder for it.
    void Configure(PixelFormat format, int width, int height, uint32_t fps_num, uint32_t fps_den);
    // Attaches to the ring when possible and keeps the request and heartbeat fresh.
    void Tick(int64_t now);
    // The stream stopped: tell the producer nobody is watching.
    void Stop();

    HANDLE frame_event() const { return shm_.is_open() ? shm_.frame_event() : nullptr; }
    uint64_t latest() const;
    bool producer_alive(int64_t now) const;

    // Writes the newest frame, or the placeholder, into dst. Returns the
    // producer frame number written, 0 for the placeholder.
    uint64_t Render(const ImageSpan& dst, int64_t now);
    void CountDelivery(bool fresh);

private:
    void LogProducerState(int64_t now);

    int camera_;
    Config config_;
    SharedFrames shm_;
    Converter converter_;
    shm::FrameRequest request_;
    std::vector<uint8_t> placeholder_;
    PixelFormat format_ = PixelFormat::Unknown;
    int width_ = 0, height_ = 0;
    int64_t next_attach_ = 0;
    bool request_published_ = false;
    bool producer_was_alive_ = false;
};

class MediaStream final : public IMFMediaStream2 {
public:
    static HRESULT Create(MediaSource* source, DWORD id, int camera, const std::vector<VideoMode>& modes,
                          MediaStream** stream);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // Called by the source.
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop(bool send_event);
    void Shutdown();
    HRESULT SetAllocator(IUnknown* allocator);
    HRESULT GetAttributes(IMFAttributes** attributes);
    DWORD id() const { return id_; }

private:
    MediaStream(MediaSource* source, DWORD id, int camera);
    ~MediaStream();
    HRESULT Initialize(const std::vector<VideoMode>& modes);
    HRESULT Begin(IMFMediaType* type);  // lock held by caller is not required
    void StartWorker();
    void StopWorker();
    void Worker();
    HRESULT Deliver(int64_t now, uint64_t* frame);

    std::atomic<ULONG> refs_{1};
    std::mutex lock_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFVideoSampleAllocator> allocator_;
    ComPtr<IMFMediaType> type_;   // what the allocator was initialised with
    bool allocator_ready_ = false;
    std::deque<ComPtr<IUnknown>> tokens_;  // pending sample requests (entries may be null)
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    DWORD id_;
    int camera_;

    // Negotiated format.
    PixelFormat format_ = PixelFormat::Unknown;
    int width_ = 0, height_ = 0;
    int64_t interval_ = 333333;
    uint32_t fps_num_ = 30, fps_den_ = 1;

    FrameFeed feed_;
    std::thread worker_;
    HANDLE stop_event_ = nullptr;
    HANDLE work_event_ = nullptr;
    uint64_t last_frame_sent_ = 0;
};

class MediaSource final : public IMFMediaSourceEx,
                          public IMFGetService,
                          public IKsControl,
                          public IMFSampleAllocatorControl {
public:
    static HRESULT Create(int camera, IMFAttributes* activate_attributes, MediaSource** source);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                       const PROPVARIANT* start_position) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl
    STDMETHODIMP_(NTSTATUS) KsProperty(PKSPROPERTY property, ULONG property_length, LPVOID data, ULONG data_length,
                                       ULONG* returned) override;
    STDMETHODIMP_(NTSTATUS) KsMethod(PKSMETHOD method, ULONG method_length, LPVOID data, ULONG data_length,
                                     ULONG* returned) override;
    STDMETHODIMP_(NTSTATUS) KsEvent(PKSEVENT event, ULONG event_length, LPVOID data, ULONG data_length,
                                    ULONG* returned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD output_stream_id, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD output_stream_id, DWORD* input_stream_id,
                                   MFSampleAllocatorUsage* usage) override;

private:
    explicit MediaSource(int camera);
    ~MediaSource();
    HRESULT Initialize(IMFAttributes* activate_attributes);
    HRESULT CheckShutdown() const;

    enum class State { Stopped, Started, Shutdown };

    std::atomic<ULONG> refs_{1};
    std::mutex lock_;
    State state_ = State::Stopped;
    int camera_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    ComPtr<MediaStream> stream_;
};

// The object Frame Server creates from the COM class: an attribute store that
// activates the media source.
class Activate final : public IMFActivate {
public:
    static HRESULT Create(int camera, Activate** activate);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFAttributes, forwarded to an inner store.
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override { return store_->GetItem(key, value); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override { return store_->GetItemType(key, type); }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override
    {
        return store_->CompareItem(key, value, result);
    }
    STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override
    {
        return store_->Compare(theirs, type, result);
    }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override { return store_->GetUINT32(key, value); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override { return store_->GetUINT64(key, value); }
    STDMETHODIMP GetDouble(REFGUID key, double* value) override { return store_->GetDouble(key, value); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override { return store_->GetGUID(key, value); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override { return store_->GetStringLength(key, length); }
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override
    {
        return store_->GetString(key, value, size, length);
    }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override
    {
        return store_->GetAllocatedString(key, value, length);
    }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return store_->GetBlobSize(key, size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* written) override
    {
        return store_->GetBlob(key, buffer, size, written);
    }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override
    {
        return store_->GetAllocatedBlob(key, buffer, size);
    }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override { return store_->GetUnknown(key, riid, ppv); }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override { return store_->SetItem(key, value); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return store_->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return store_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override { return store_->SetUINT32(key, value); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override { return store_->SetUINT64(key, value); }
    STDMETHODIMP SetDouble(REFGUID key, double value) override { return store_->SetDouble(key, value); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override { return store_->SetGUID(key, value); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override { return store_->SetString(key, value); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override
    {
        return store_->SetBlob(key, buffer, size);
    }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override { return store_->SetUnknown(key, value); }
    STDMETHODIMP LockStore() override { return store_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return store_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* count) override { return store_->GetCount(count); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override
    {
        return store_->GetItemByIndex(index, key, value);
    }
    STDMETHODIMP CopyAllItems(IMFAttributes* destination) override { return store_->CopyAllItems(destination); }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    explicit Activate(int camera);
    ~Activate();

    std::atomic<ULONG> refs_{1};
    std::mutex lock_;
    int camera_;
    ComPtr<IMFAttributes> store_;
    ComPtr<MediaSource> source_;
};

// Media Foundation subtype <-> vcam pixel format.
PixelFormat FormatFromSubtype(REFGUID subtype);
GUID SubtypeFromFormat(PixelFormat format);

}  // namespace vcam::win
