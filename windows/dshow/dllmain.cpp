// COM entry points of vcamdshow.dll, and its registration as video capture
// devices (run by `vcam install` through regsvr32, once per architecture).
#include "dshow.h"

#include <cstring>
#include <mutex>

namespace vcam::win {

std::atomic<long> g_objects{0};

namespace {

HMODULE g_module = nullptr;

class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(int camera) : camera_(camera) { g_objects++; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        static std::once_flag log_once;
        std::call_once(log_once, [] { LogInit(L"vcamdshow", true); });
        Filter* filter = new (std::nothrow) Filter(camera_);
        if (!filter) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) {
            g_objects++;
        } else {
            g_objects--;
        }
        return S_OK;
    }

private:
    ~ClassFactory() { g_objects--; }

    std::atomic<ULONG> refs_{1};
    int camera_;
};

// HKLM\SOFTWARE\Classes\CLSID\{clsid} in this process's registry view: the
// 32-bit filter registers for 32-bit applications, the 64-bit one for 64-bit.
std::wstring ClassKey(int camera)
{
    return L"SOFTWARE\\Classes\\CLSID\\" + GuidString(DirectShowClsid(camera));
}

LSTATUS WriteValue(const std::wstring& path, const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                     nullptr);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    return status;
}

void DeleteClassKey(int camera)
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClassKey(camera).c_str());
}

// COM for the registration functions; regsvr32 has usually initialised it already.
class ComScope {
public:
    ComScope() : ok_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (ok_) {
            CoUninitialize();
        }
    }

private:
    bool ok_;
};

HRESULT CreateMapper(IFilterMapper2** mapper)
{
    return CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                            reinterpret_cast<void**>(mapper));
}

}  // namespace
}  // namespace vcam::win

using namespace vcam::win;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    int camera = 0;
    if (!CameraFromDirectShowClsid(clsid, &camera)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    ClassFactory* factory = new (std::nothrow) ClassFactory(camera);
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return g_objects.load() == 0 ? S_OK : S_FALSE;
}

// One video capture device per installed camera, named as configured; the
// slots beyond the configured count are removed.
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const Config config = LoadConfig();
    ComScope com;
    IFilterMapper2* mapper = nullptr;
    HRESULT hr = CreateMapper(&mapper);
    if (FAILED(hr)) {
        return hr;
    }
    const REGPINTYPES types = {&MEDIATYPE_Video, &MEDIASUBTYPE_NULL};
    REGFILTERPINS2 pin;
    memset(&pin, 0, sizeof(pin));
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes = 1;
    pin.lpMediaType = &types;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
    REGFILTER2 filter;
    memset(&filter, 0, sizeof(filter));
    filter.dwVersion = 2;
    // Never picked by intelligent connect, only by apps that list capture devices.
    filter.dwMerit = MERIT_DO_NOT_USE;
    filter.cPins2 = 1;
    filter.rgPins2 = &pin;

    for (int i = 0; i < kMaxCameras && SUCCEEDED(hr); i++) {
        const CLSID clsid = DirectShowClsid(i);
        if (i >= config.cameras) {
            mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, clsid);
            DeleteClassKey(i);
            continue;
        }
        const std::wstring key = ClassKey(i);
        LSTATUS status = WriteValue(key, nullptr, config.names[i]);
        if (status == ERROR_SUCCESS) {
            status = WriteValue(key + L"\\InprocServer32", nullptr, path);
        }
        if (status == ERROR_SUCCESS) {
            status = WriteValue(key + L"\\InprocServer32", L"ThreadingModel", L"Both");
        }
        hr = HRESULT_FROM_WIN32(status);
        if (SUCCEEDED(hr)) {
            hr = mapper->RegisterFilter(clsid, config.names[i].c_str(), nullptr, &CLSID_VideoInputDeviceCategory,
                                        nullptr, &filter);
        }
    }
    mapper->Release();
    return hr;
}

STDAPI DllUnregisterServer()
{
    ComScope com;
    IFilterMapper2* mapper = nullptr;
    const HRESULT hr = CreateMapper(&mapper);
    for (int i = 0; i < kMaxCameras; i++) {
        if (mapper) {
            mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, DirectShowClsid(i));
        }
        DeleteClassKey(i);
    }
    if (mapper) {
        mapper->Release();
    }
    return SUCCEEDED(hr) ? S_OK : hr;
}
