#include "source.h"

namespace vcam::win {

namespace {

PROPVARIANT TimeVariant()
{
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = MFGetSystemTime();
    return v;
}

// Camera profiles: the legacy profile (all modes up to 30 fps) is mandatory for
// profile-aware apps; the high frame rate profile exposes the 60 fps modes.
HRESULT AddSensorProfiles(IMFAttributes* attributes)
{
    ComPtr<IMFSensorProfileCollection> collection;
    ComPtr<IMFSensorProfile> legacy, high;
    HRESULT hr = MFCreateSensorProfileCollection(&collection);
    if (SUCCEEDED(hr)) hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy);
    if (SUCCEEDED(hr)) hr = legacy->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
    if (SUCCEEDED(hr)) hr = collection->AddProfile(legacy.Get());
    if (SUCCEEDED(hr)) hr = MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &high);
    if (SUCCEEDED(hr)) hr = high->AddProfileFilter(0, L"((RES==;FRT>=60,1;SUT==))");
    if (SUCCEEDED(hr)) hr = collection->AddProfile(high.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection.Get());
    return hr;
}

}  // namespace

MediaSource::MediaSource(int camera) : camera_(camera)
{
    g_objects++;
}

MediaSource::~MediaSource()
{
    g_objects--;
}

HRESULT MediaSource::Create(int camera, IMFAttributes* activate_attributes, MediaSource** source)
{
    MediaSource* s = new (std::nothrow) MediaSource(camera);
    if (!s) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = s->Initialize(activate_attributes);
    if (FAILED(hr)) {
        Log("camera %d: media source initialisation failed (0x%08lX)", camera + 1, static_cast<unsigned long>(hr));
        s->Shutdown();
        s->Release();
        return hr;
    }
    *source = s;
    return S_OK;
}

HRESULT MediaSource::Initialize(IMFAttributes* activate_attributes)
{
    // Frame Server passes device information through the activate's attributes;
    // the source's own store must carry a copy.
    HRESULT hr = MFCreateAttributes(&attributes_, 8);
    if (SUCCEEDED(hr) && activate_attributes) {
        hr = activate_attributes->CopyAllItems(attributes_.Get());
    }
    if (FAILED(hr)) {
        return hr;
    }
    const HRESULT profiles = AddSensorProfiles(attributes_.Get());
    if (FAILED(profiles)) {
        Log("camera %d: camera profiles unavailable (0x%08lX)", camera_ + 1, static_cast<unsigned long>(profiles));
    }

    const Config config = LoadConfig();
    const std::vector<VideoMode> modes = StandardModes(config.max_width, config.max_height, config.allow_60fps);
    hr = MediaStream::Create(this, 0, camera_, modes, &stream_);
    ComPtr<IMFStreamDescriptor> stream_descriptor;
    if (SUCCEEDED(hr)) hr = stream_->GetStreamDescriptor(&stream_descriptor);
    if (SUCCEEDED(hr)) {
        IMFStreamDescriptor* list[] = {stream_descriptor.Get()};
        hr = MFCreatePresentationDescriptor(1, list, &descriptor_);
    }
    if (SUCCEEDED(hr)) hr = MFCreateEventQueue(&queue_);
    if (SUCCEEDED(hr)) {
        Log("camera %d: media source created (%zu modes, up to %ux%u)", camera_ + 1, modes.size() * 3,
            config.max_width, config.max_height);
    }
    return hr;
}

HRESULT MediaSource::CheckShutdown() const
{
    return state_ == State::Shutdown ? MF_E_SHUTDOWN : S_OK;
}

// --- IUnknown ----------------------------------------------------------------------------

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
        riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef()
{
    return ++refs_;
}

STDMETHODIMP_(ULONG) MediaSource::Release()
{
    const ULONG n = --refs_;
    if (n == 0) {
        delete this;
    }
    return n;
}

// --- IMFMediaEventGenerator --------------------------------------------------------------------

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(lock_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) {
            return hr;
        }
        queue = queue_;
    }
    return queue->GetEvent(flags, event);  // may block: not under the lock
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value)
{
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : queue_->QueueEventParamVar(type, extended, status, value);
}

// --- IMFMediaSource ------------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics)
{
    if (!characteristics) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        *characteristics = MFMEDIASOURCE_IS_LIVE;
    }
    return hr;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
{
    if (!descriptor) {
        return E_POINTER;
    }
    *descriptor = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                                const PROPVARIANT* start_position)
{
    if (!descriptor || !start_position) {
        return E_INVALIDARG;
    }
    if (time_format && *time_format != GUID_NULL) {
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
        return hr;
    }
    DWORD count = 0;
    hr = descriptor->GetStreamDescriptorCount(&count);
    if (FAILED(hr) || count != 1) {
        return E_INVALIDARG;
    }
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> requested;
    hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &requested);
    DWORD id = 0;
    if (SUCCEEDED(hr)) hr = requested->GetStreamIdentifier(&id);
    if (FAILED(hr)) {
        return hr;
    }
    if (id != stream_->id()) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    BOOL was_selected = FALSE;
    ComPtr<IMFStreamDescriptor> ours;
    hr = descriptor_->GetStreamDescriptorByIndex(0, &was_selected, &ours);
    if (FAILED(hr)) {
        return hr;
    }

    if (selected) {
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        hr = requested->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
        if (SUCCEEDED(hr)) hr = descriptor_->SelectStream(0);
        if (SUCCEEDED(hr)) {
            hr = queue_->QueueEventParamUnk(was_selected ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                            static_cast<IMFMediaStream*>(stream_.Get()));
        }
        if (SUCCEEDED(hr)) hr = stream_->Start(type.Get());
        if (FAILED(hr)) {
            return hr;
        }
    } else if (was_selected) {
        descriptor_->DeselectStream(0);
        stream_->Stop(true);
    }

    PROPVARIANT time = TimeVariant();
    hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
    PropVariantClear(&time);
    if (SUCCEEDED(hr)) {
        state_ = State::Started;
    }
    return hr;
}

STDMETHODIMP MediaSource::Stop()
{
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
        return hr;
    }
    if (state_ != State::Started) {
        return MF_E_INVALID_STATE_TRANSITION;
    }
    stream_->Stop(true);
    descriptor_->DeselectStream(0);
    PROPVARIANT time = TimeVariant();
    hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
    PropVariantClear(&time);
    state_ = State::Stopped;
    return hr;
}

STDMETHODIMP MediaSource::Pause()
{
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : MF_E_INVALID_STATE_TRANSITION;  // cameras do not pause
}

STDMETHODIMP MediaSource::Shutdown()
{
    std::lock_guard<std::mutex> lock(lock_);
    if (state_ == State::Shutdown) {
        return MF_E_SHUTDOWN;
    }
    state_ = State::Shutdown;
    if (stream_) {
        stream_->Shutdown();
        stream_.Reset();
    }
    if (queue_) {
        queue_->Shutdown();
        queue_.Reset();
    }
    descriptor_.Reset();
    attributes_.Reset();
    Log("camera %d: media source shut down", camera_ + 1);
    return S_OK;
}

// --- IMFMediaSourceEx -------------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes)
{
    if (!attributes) {
        return E_POINTER;
    }
    *attributes = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    // The pipeline expects the store itself, not a copy: it may add items to it.
    return FAILED(hr) ? hr : attributes_.CopyTo(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes)
{
    if (!attributes) {
        return E_POINTER;
    }
    *attributes = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
        return hr;
    }
    return stream_id == stream_->id() ? stream_->GetAttributes(attributes) : MF_E_INVALIDSTREAMNUMBER;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    // Frames are produced in system memory; the pipeline uploads them when it
    // needs GPU surfaces. The return value is advisory.
    return E_NOTIMPL;
}

// --- IMFGetService ------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object)
{
    if (!object) {
        return E_POINTER;
    }
    *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// --- IKsControl ----------------------------------------------------------------------------------------

// No camera controls (exposure, zoom, ...): answer like a driver without the property set.
STDMETHODIMP_(NTSTATUS) MediaSource::KsProperty(PKSPROPERTY property, ULONG property_length, LPVOID, ULONG,
                                                ULONG* returned)
{
    if (!property || property_length < sizeof(KSPROPERTY) || !returned) {
        return E_INVALIDARG;
    }
    *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaSource::KsMethod(PKSMETHOD method, ULONG method_length, LPVOID, ULONG, ULONG* returned)
{
    if (!method || method_length < sizeof(KSMETHOD) || !returned) {
        return E_INVALIDARG;
    }
    *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned)
{
    if (returned) {
        *returned = 0;
    }
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// --- IMFSampleAllocatorControl ---------------------------------------------------------------------------

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD output_stream_id, IUnknown* allocator)
{
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
        return hr;
    }
    return output_stream_id == stream_->id() ? stream_->SetAllocator(allocator) : MF_E_INVALIDSTREAMNUMBER;
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD output_stream_id, DWORD* input_stream_id,
                                            MFSampleAllocatorUsage* usage)
{
    if (!input_stream_id || !usage) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(lock_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
        return hr;
    }
    if (output_stream_id != stream_->id()) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    *input_stream_id = output_stream_id;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

}  // namespace vcam::win
