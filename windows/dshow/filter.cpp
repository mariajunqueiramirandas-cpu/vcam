// The capture filter: states, clock, graph membership, and its single pin.
#include "dshow.h"

#include <cwchar>

namespace vcam::win {

namespace {

class PinEnum final : public IEnumPins {
public:
    PinEnum(IPin* pin, ULONG position) : pin_(pin), position_(position)
    {
        pin_->AddRef();
        g_objects++;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
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

    STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override
    {
        if (!pins || (count > 1 && !fetched)) {
            return E_POINTER;
        }
        ULONG n = 0;
        while (n < count && position_ < 1) {
            pins[n] = pin_;
            pin_->AddRef();
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
        return position_ <= 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        position_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** clone) override
    {
        if (!clone) {
            return E_POINTER;
        }
        *clone = new (std::nothrow) PinEnum(pin_, position_);
        return *clone ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~PinEnum()
    {
        pin_->Release();
        g_objects--;
    }

    std::atomic<ULONG> refs_{1};
    IPin* pin_;
    ULONG position_;
};

}  // namespace

Filter::Filter(int camera) : camera_(camera), pin_(this, camera)
{
    g_objects++;
}

Filter::~Filter()
{
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        state_ = State_Stopped;
        pin_.Inactive();
        pin_.ReleaseConnection();
    }
    if (clock_) {
        clock_->Release();
    }
    g_objects--;
}

STDMETHODIMP Filter::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) Filter::AddRef()
{
    return ++refs_;
}

STDMETHODIMP_(ULONG) Filter::Release()
{
    const ULONG n = --refs_;
    if (n == 0) {
        delete this;
    }
    return n;
}

STDMETHODIMP Filter::GetClassID(CLSID* clsid)
{
    if (!clsid) {
        return E_POINTER;
    }
    *clsid = DirectShowClsid(camera_);
    return S_OK;
}

// --- states ---------------------------------------------------------------------------------

STDMETHODIMP Filter::Stop()
{
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (state_ == State_Stopped) {
        return S_OK;
    }
    state_ = State_Stopped;
    pin_.Inactive();
    return S_OK;
}

STDMETHODIMP Filter::Pause()
{
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (state_ == State_Stopped) {
        const HRESULT hr = pin_.Active();
        if (FAILED(hr)) {
            return hr;
        }
    }
    state_ = State_Paused;
    pin_.Wake();
    return S_OK;
}

STDMETHODIMP Filter::Run(REFERENCE_TIME start)
{
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (state_ == State_Stopped) {
        const HRESULT hr = pin_.Active();
        if (FAILED(hr)) {
            return hr;
        }
    }
    start_ = start;
    state_ = State_Running;
    pin_.Wake();
    return S_OK;
}

STDMETHODIMP Filter::GetState(DWORD, FILTER_STATE* state)
{
    if (!state) {
        return E_POINTER;
    }
    *state = state_.load();
    // A live source has nothing to cue while paused: renderers must not wait for a frame.
    return *state == State_Paused ? VFW_S_CANT_CUE : S_OK;
}

STDMETHODIMP Filter::SetSyncSource(IReferenceClock* clock)
{
    std::lock_guard<std::mutex> lock(clock_lock_);
    if (clock) {
        clock->AddRef();
    }
    if (clock_) {
        clock_->Release();
    }
    clock_ = clock;
    return S_OK;
}

STDMETHODIMP Filter::GetSyncSource(IReferenceClock** clock)
{
    if (!clock) {
        return E_POINTER;
    }
    *clock = reference_clock();
    return S_OK;
}

IReferenceClock* Filter::reference_clock()
{
    std::lock_guard<std::mutex> lock(clock_lock_);
    if (clock_) {
        clock_->AddRef();
    }
    return clock_;
}

// --- graph -----------------------------------------------------------------------------------

STDMETHODIMP Filter::EnumPins(IEnumPins** pins)
{
    if (!pins) {
        return E_POINTER;
    }
    *pins = new (std::nothrow) PinEnum(static_cast<IPin*>(&pin_), 0);
    return *pins ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Filter::FindPin(LPCWSTR id, IPin** pin)
{
    if (!id || !pin) {
        return E_POINTER;
    }
    if (wcscmp(id, L"Capture") != 0) {
        *pin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    *pin = static_cast<IPin*>(&pin_);
    (*pin)->AddRef();
    return S_OK;
}

STDMETHODIMP Filter::QueryFilterInfo(FILTER_INFO* info)
{
    if (!info) {
        return E_POINTER;
    }
    std::lock_guard<std::recursive_mutex> lock(lock_);
    wcsncpy_s(info->achName, MAX_FILTER_NAME, name_.c_str(), _TRUNCATE);
    info->pGraph = graph_;
    if (graph_) {
        graph_->AddRef();
    }
    return S_OK;
}

STDMETHODIMP Filter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name)
{
    std::lock_guard<std::recursive_mutex> lock(lock_);
    graph_ = graph;
    name_ = name ? name : L"";
    return S_OK;
}

STDMETHODIMP Filter::QueryVendorInfo(LPWSTR*)
{
    return E_NOTIMPL;
}

STDMETHODIMP_(ULONG) Filter::GetMiscFlags()
{
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

}  // namespace vcam::win
