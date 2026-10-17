#include "source.h"

namespace vcam::win {

Activate::Activate(int camera) : camera_(camera)
{
    g_objects++;
}

Activate::~Activate()
{
    g_objects--;
}

HRESULT Activate::Create(int camera, Activate** activate)
{
    static std::once_flag log_once;
    std::call_once(log_once, [] { LogInit(L"vcamsource"); });

    Activate* a = new (std::nothrow) Activate(camera);
    if (!a) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = MFCreateAttributes(&a->store_, 4);
    if (FAILED(hr)) {
        a->Release();
        return hr;
    }
    *activate = a;
    return S_OK;
}

STDMETHODIMP Activate::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Activate::AddRef()
{
    return ++refs_;
}

STDMETHODIMP_(ULONG) Activate::Release()
{
    const ULONG n = --refs_;
    if (n == 0) {
        delete this;
    }
    return n;
}

STDMETHODIMP Activate::ActivateObject(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    if (!source_) {
        // The attributes Frame Server set on us (device information) are copied into the source.
        const HRESULT hr = MediaSource::Create(camera_, store_.Get(), &source_);
        if (FAILED(hr)) {
            return hr;
        }
    }
    return source_->QueryInterface(riid, ppv);
}

STDMETHODIMP Activate::ShutdownObject()
{
    // Frame Server shuts the source down itself.
    return S_OK;
}

STDMETHODIMP Activate::DetachObject()
{
    std::lock_guard<std::mutex> lock(lock_);
    source_.Reset();
    return S_OK;
}

}  // namespace vcam::win
