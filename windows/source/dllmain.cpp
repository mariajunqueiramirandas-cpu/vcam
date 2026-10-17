// COM entry points of vcamsource.dll.
#include "source.h"

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
        Activate* activate = nullptr;
        HRESULT hr = Activate::Create(camera_, &activate);
        if (SUCCEEDED(hr)) {
            hr = activate->QueryInterface(riid, ppv);
            activate->Release();
        }
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
    if (!CameraFromClsid(clsid, &camera)) {
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

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return HRESULT_FROM_WIN32(RegisterSourceClasses(path));
}

STDAPI DllUnregisterServer()
{
    UnregisterSourceClasses();
    return S_OK;
}
