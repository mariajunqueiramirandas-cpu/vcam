// Installing, configuring and inspecting vcam.
#include "app.h"

#include <aclapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

#include "vcam/modes.h"

using Microsoft::WRL::ComPtr;

namespace vcam::app {

namespace {

// Device property vcam puts on its cameras: the camera slot (0-based).
const DEVPROPKEY kCameraSlotKey = {{0x2d0f7c34, 0x91b6, 0x4c4e, {0xb5, 0x3e, 0x6a, 0x20, 0x8d, 0x1c, 0x47, 0x9a}}, 2};

constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\vcam";
constexpr wchar_t kEnvironmentKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

// --- the virtual camera API, loaded at run time so vcam starts (and explains)
// on systems without it -------------------------------------------------------------

class VirtualCameraApi {
public:
    using CreateFn = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess,
                                              LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
    using SupportedFn = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, BOOL*);

    VirtualCameraApi()
    {
        module_ = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_) {
            create_ = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(module_, "MFCreateVirtualCamera")));
            supported_ = reinterpret_cast<SupportedFn>(
                reinterpret_cast<void*>(GetProcAddress(module_, "MFIsVirtualCameraTypeSupported")));
        }
    }
    VirtualCameraApi(const VirtualCameraApi&) = delete;
    VirtualCameraApi& operator=(const VirtualCameraApi&) = delete;
    ~VirtualCameraApi()
    {
        if (module_) {
            FreeLibrary(module_);
        }
    }

    bool available() const
    {
        if (!create_ || !supported_) {
            return false;
        }
        BOOL supported = FALSE;
        return SUCCEEDED(supported_(MFVirtualCameraType_SoftwareCameraSource, &supported)) && supported;
    }

    HRESULT Open(int index, const std::wstring& name, IMFVirtualCamera** camera) const
    {
        if (!create_) {
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        return create_(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                       MFVirtualCameraAccess_AllUsers, name.c_str(), win::GuidString(win::SourceClsid(index)).c_str(),
                       nullptr, 0, camera);
    }

private:
    HMODULE module_ = nullptr;
    CreateFn create_ = nullptr;
    SupportedFn supported_ = nullptr;
};

class MediaFoundation {
public:
    MediaFoundation()
    {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf_ = SUCCEEDED(MFStartup(MF_VERSION));
    }
    MediaFoundation(const MediaFoundation&) = delete;
    MediaFoundation& operator=(const MediaFoundation&) = delete;
    ~MediaFoundation()
    {
        if (mf_) {
            MFShutdown();
        }
        if (com_) {
            CoUninitialize();
        }
    }

private:
    bool com_ = false;
    bool mf_ = false;
};

DWORD WindowsBuild()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn && fn(&info) == 0 ? info.dwBuildNumber : 0;
}

// Registers (or re-registers) camera `index`: system lifetime, all users, so
// it exists for every app whether or not a producer runs.
HRESULT RegisterCamera(const VirtualCameraApi& api, int index, const std::wstring& name)
{
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = api.Open(index, name, &camera);
    if (FAILED(hr)) {
        return hr;
    }
    const UINT32 slot = static_cast<UINT32>(index);
    camera->AddProperty(&kCameraSlotKey, DEVPROP_TYPE_UINT32, reinterpret_cast<const BYTE*>(&slot), sizeof(slot));
    hr = camera->Start(nullptr);
    camera->Shutdown();
    return hr;
}

HRESULT RemoveCamera(const VirtualCameraApi& api, int index, const std::wstring& name)
{
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = api.Open(index, name, &camera);
    if (FAILED(hr)) {
        return hr;
    }
    hr = camera->Remove();
    camera->Shutdown();
    return hr;
}

std::wstring ExecutablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring Directory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

std::wstring FullPath(const std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 2, buffer, nullptr);
    return n && n < MAX_PATH * 2 ? std::wstring(buffer) : path;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    std::wstring x = Lower(FullPath(a)), y = Lower(FullPath(b));
    while (!x.empty() && x.back() == L'\\') x.pop_back();
    while (!y.empty() && y.back() == L'\\') y.pop_back();
    return x == y;
}

bool FileExists(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ProgramFilesDir()
{
    PWSTR folder = nullptr;
    std::wstring result = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) {
        result = folder;
    }
    CoTaskMemFree(folder);
    return result;
}

// Copies a file over one that may be in use (Frame Server keeps the media
// source loaded): a loaded DLL can be renamed, so move it aside and delete it
// at the next restart.
bool ReplaceFileInUse(const std::wstring& from, const std::wstring& to, bool* deferred)
{
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED && error != ERROR_USER_MAPPED_FILE) {
        return false;
    }
    const std::wstring aside = to + L".old-" + std::to_wstring(GetTickCount64());
    if (!MoveFileExW(to.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return false;
    }
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    *deferred = true;
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

void RemoveMarkOfTheWeb(const std::wstring& path)
{
    // Downloaded files carry a Zone.Identifier stream; Frame Server refuses to
    // load a media source that has one.
    DeleteFileW((path + L":Zone.Identifier").c_str());
}

bool ApplySddl(const std::wstring& path, const wchar_t* sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) &&
              SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                    dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(sd);
    return ok;
}

// %ProgramData%\vcam: readable by everyone (the placeholder image lives here),
// logs\ also writable by LOCAL SERVICE, the account Frame Server runs as.
void CreateDataDirectories()
{
    const std::wstring data = win::DataDirectory();
    CreateDirectoryW(data.c_str(), nullptr);
    ApplySddl(data, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;LS)");
    const std::wstring logs = data + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    ApplySddl(logs, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1301bf;;;LS)");
}

std::vector<std::wstring> SplitPath(const std::wstring& value)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(L';', start);
        const std::wstring part = value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

// Adds or removes `dir` in the machine PATH, keeping REG_EXPAND_SZ intact.
void UpdateMachinePath(const std::wstring& dir, bool add)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEnvironmentKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD type = REG_EXPAND_SZ, bytes = 0;
    std::wstring value;
    if (RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        value.resize(bytes / sizeof(wchar_t) + 1);
        RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
        value.resize(wcslen(value.c_str()));
    } else {
        type = REG_EXPAND_SZ;
    }
    std::vector<std::wstring> parts = SplitPath(value);
    const auto same = [&](const std::wstring& p) { return SamePath(p, dir); };
    const bool present = std::any_of(parts.begin(), parts.end(), same);
    if (add == present) {
        RegCloseKey(key);
        return;
    }
    if (add) {
        parts.push_back(dir);
    } else {
        parts.erase(std::remove_if(parts.begin(), parts.end(), same), parts.end());
    }
    std::wstring updated;
    for (const auto& p : parts) {
        updated += (updated.empty() ? L"" : L";") + p;
    }
    RegSetValueExW(key, L"Path", 0, type, reinterpret_cast<const BYTE*>(updated.c_str()),
                   static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 2000, &result);
}

void WriteUninstallEntry(const std::wstring& dir)
{
    const std::wstring exe = L"\"" + dir + L"\\vcam.exe\"";
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayName", L"vcam virtual camera");
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", Wide(VCAM_VERSION_STRING));
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Publisher", L"vcam");
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", dir);
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayIcon", dir + L"\\vcam.exe");
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString", exe + L" uninstall");
    win::RegWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"QuietUninstallString", exe + L" uninstall");
    win::RegWriteDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoModify", 1);
    win::RegWriteDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoRepair", 1);
    win::RegWriteDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"EstimatedSize", 2048);
}

struct RegisteredCamera {
    int slot = -1;  // vcam camera slot, -1 if not ours
    std::wstring name;
    std::wstring link;
};

std::vector<RegisteredCamera> VideoDevices(const win::Config& config)
{
    std::vector<RegisteredCamera> result;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1)) ||
        FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return result;
    }
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count))) {
        return result;
    }
    for (UINT32 i = 0; i < count; i++) {
        RegisteredCamera cam;
        wchar_t* text = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &text, &length))) {
            cam.name = text;
            CoTaskMemFree(text);
        }
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &text,
                                                     &length))) {
            cam.link = text;
            CoTaskMemFree(text);
        }
        DEVPROPTYPE type = 0;
        UINT32 slot = 0;
        ULONG size = sizeof(slot);
        if (!cam.link.empty() &&
            CM_Get_Device_Interface_PropertyW(cam.link.c_str(), &kCameraSlotKey, &type, reinterpret_cast<BYTE*>(&slot),
                                              &size, 0) == CR_SUCCESS &&
            type == DEVPROP_TYPE_UINT32 && slot < static_cast<UINT32>(win::kMaxCameras)) {
            cam.slot = static_cast<int>(slot);
        } else {
            // Fall back to the name ("<name>" possibly followed by Windows' suffix).
            for (int s = 0; s < win::kMaxCameras && cam.slot < 0; s++) {
                if (s < config.cameras && cam.name.rfind(config.names[s], 0) == 0 &&
                    (cam.name.size() == config.names[s].size() || cam.name[config.names[s].size()] == L' ')) {
                    cam.slot = s;
                }
            }
        }
        result.push_back(cam);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return result;
}

bool StopService(const wchar_t* name)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    bool stopped = false;
    if (service) {
        SERVICE_STATUS status{};
        if (ControlService(service, SERVICE_CONTROL_STOP, &status) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
            for (int i = 0; i < 100 && QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED;
                 i++) {
                Sleep(100);
            }
            stopped = status.dwCurrentState == SERVICE_STOPPED;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return stopped;
}

std::string ProcessName(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return "pid " + std::to_string(pid);
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string name = "pid " + std::to_string(pid);
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring p(path);
        name = Utf8(p.substr(p.find_last_of(L'\\') + 1)) + " (pid " + std::to_string(pid) + ")";
    }
    CloseHandle(process);
    return name;
}

std::string FormatResolution(uint32_t w, uint32_t h)
{
    return std::to_string(w) + "x" + std::to_string(h);
}

// --- DirectShow backend (Windows 10) ---------------------------------------------------

// CLSID_VideoInputDeviceCategory: where DirectShow lists capture devices.
constexpr wchar_t kVideoInputCategory[] = L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";

std::wstring SystemDirectory(const wchar_t* sub)
{
    wchar_t windows[MAX_PATH] = {};
    const UINT n = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    return std::wstring(n && n < MAX_PATH ? windows : L"C:\\Windows") + L"\\" + sub;
}

// Runs a program without a window and returns its exit code (or -1).
int RunHidden(const std::wstring& command_line, DWORD timeout_ms)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = command_line;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return -1;
    }
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// The filters register themselves (DllRegisterServer reads the configuration:
// one capture device per camera, with its name). regsvr32 from System32 runs
// the 64-bit filter, the one from SysWOW64 the 32-bit filter for 32-bit apps.
bool RunRegsvr32(const std::wstring& dir, bool unregister, std::string* error)
{
    struct Item {
        const wchar_t* dll;
        const wchar_t* system;
        bool required;
    };
    static const Item items[] = {{L"vcamdshow.dll", L"System32", true}, {L"vcamdshow32.dll", L"SysWOW64", false}};
    bool ok = true;
    for (const Item& item : items) {
        const std::wstring dll = dir + L"\\" + item.dll;
        const std::wstring regsvr32 = SystemDirectory(item.system) + L"\\regsvr32.exe";
        if (!FileExists(dll) || !FileExists(regsvr32)) {
            if (item.required && !unregister) {
                *error = Utf8(dll) + " is missing";
                ok = false;
            }
            continue;
        }
        const int code = RunHidden(L"\"" + regsvr32 + L"\" /s " + (unregister ? L"/u " : L"") + L"\"" + dll + L"\"", 30000);
        if (code != 0 && !unregister) {
            *error = "registering " + Utf8(item.dll) + " failed (regsvr32 exit code " + std::to_string(code) + ")";
            ok = false;
        }
    }
    return ok;
}

bool DirectShowRegistered(int index, bool wow32)
{
    const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + std::wstring(kVideoInputCategory) + L"\\Instance\\" +
                             win::GuidString(win::DirectShowClsid(index));
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | (wow32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY), &h) !=
        ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(h);
    return true;
}

// Removes every DirectShow registration of vcam from both registry views, in
// case the filters themselves could not (files already gone).
void RemoveDirectShowKeys()
{
    for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY classes = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID", 0, KEY_ALL_ACCESS | view, &classes) !=
            ERROR_SUCCESS) {
            continue;
        }
        for (int i = 0; i < win::kMaxCameras; i++) {
            const std::wstring clsid = win::GuidString(win::DirectShowClsid(i));
            RegDeleteTreeW(classes, clsid.c_str());
            RegDeleteKeyExW(classes, clsid.c_str(), view, 0);
            const std::wstring instance = std::wstring(kVideoInputCategory) + L"\\Instance\\" + clsid;
            RegDeleteTreeW(classes, instance.c_str());
            RegDeleteKeyExW(classes, instance.c_str(), view, 0);
        }
        RegCloseKey(classes);
    }
}

// --- Start menu --------------------------------------------------------------------------------

std::wstring StartMenuShortcut()
{
    PWSTR folder = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &folder))) {
        path = std::wstring(folder) + L"\\VCam.lnk";
    }
    CoTaskMemFree(folder);
    return path;
}

void CreateStartMenuShortcut(const std::wstring& target)
{
    const std::wstring link = StartMenuShortcut();
    ComPtr<IShellLinkW> shell_link;
    ComPtr<IPersistFile> file;
    if (link.empty() || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
        return;
    }
    shell_link->SetPath(target.c_str());
    shell_link->SetWorkingDirectory(Directory(target).c_str());
    shell_link->SetDescription(L"VCam virtual camera");
    if (SUCCEEDED(shell_link.As(&file))) {
        file->Save(link.c_str(), TRUE);
    }
}

}  // namespace

// --- commands ---------------------------------------------------------------------------------

bool VirtualCamerasSupported()
{
    MediaFoundation mf;
    VirtualCameraApi api;
    return api.available();
}

int CmdInstall(const InstallOptions& options)
{
    if (!RequireElevation("vcam install")) {
        return 1;
    }
    MediaFoundation mf;
    VirtualCameraApi api;
    const bool mf_supported = api.available();
    win::Config previous = win::LoadConfig();
    const bool upgrade = !previous.install_dir.empty();
    win::Backend backend = mf_supported ? win::Backend::MediaFoundation : win::Backend::DirectShow;
    if (options.backend >= 0) {
        backend = static_cast<win::Backend>(options.backend);
    } else if (upgrade && (previous.backend == win::Backend::DirectShow || mf_supported)) {
        backend = previous.backend;  // keep what an earlier install chose
    }
    if (backend == win::Backend::MediaFoundation && !mf_supported) {
        PrintError("Media Foundation virtual cameras need Windows 11 (build 22000 or later; this is build %lu). "
                   "Install with --backend directshow instead.\n",
                   WindowsBuild());
        return 1;
    }

    win::Config config = previous;
    const std::wstring from_dir = Directory(ExecutablePath());
    const std::wstring dir = ProgramFilesDir() + L"\\vcam";
    const wchar_t* backend_dll = backend == win::Backend::MediaFoundation ? L"vcamsource.dll" : L"vcamdshow.dll";

    // 1. Files (the setup program copies them itself).
    Print("Installing to %s (%s backend)\n", Utf8(dir).c_str(), win::BackendName(backend));
    CreateDirectoryW(dir.c_str(), nullptr);
    bool deferred = false;
    if (!options.managed && !SamePath(from_dir, dir)) {
        struct Item {
            const wchar_t* path;  // relative to the package and to the install directory
            bool required;
        };
        static const Item items[] = {
            {L"vcam.exe", true},          {L"vcam-gui.exe", false},     {L"vcamsource.dll", false},
            {L"vcamdshow.dll", false},    {L"vcamdshow32.dll", false},  {L"vcam.dll", false},
            {L"sdk\\vcam.h", false},      {L"sdk\\vcam.lib", false},    {L"python\\vcam.py", false},
            {L"python\\example.py", false}, {L"README.md", false},
        };
        for (const Item& item : items) {
            const std::wstring src = from_dir + L"\\" + item.path;
            const std::wstring dst = dir + L"\\" + item.path;
            if (!FileExists(src)) {
                if (item.required) {
                    PrintError("%s is missing next to vcam.exe; run install from the extracted package\n",
                               Utf8(item.path).c_str());
                    return 1;
                }
                continue;
            }
            CreateDirectoryW(Directory(dst).c_str(), nullptr);
            if (!ReplaceFileInUse(src, dst, &deferred)) {
                PrintError("copying %s failed: %s\n", Utf8(item.path).c_str(), ErrorText(GetLastError()).c_str());
                return 1;
            }
            RemoveMarkOfTheWeb(dst);
        }
    } else {
        for (const wchar_t* f : {L"vcamsource.dll", L"vcamdshow.dll", L"vcamdshow32.dll"}) {
            RemoveMarkOfTheWeb(dir + L"\\" + f);
        }
    }
    if (!FileExists(dir + L"\\" + backend_dll)) {
        PrintError("%s\\%s is missing\n", Utf8(dir).c_str(), Utf8(backend_dll).c_str());
        return 1;
    }
    CreateDataDirectories();

    // 2. Configuration (the DirectShow filters read it while registering).
    config.install_dir = dir;
    config.cameras = options.cameras > 0 ? options.cameras : std::max(previous.cameras, 1);
    config.backend = backend;
    config.managed = options.managed;
    for (int i = 0; i < win::kMaxCameras; i++) {
        if (i < static_cast<int>(options.names.size()) && !options.names[i].empty()) {
            config.names[i] = options.names[i];
        }
    }
    if (options.max_width) {
        config.max_width = options.max_width;
        config.max_height = options.max_height;
    }
    if (options.allow_60fps >= 0) {
        config.allow_60fps = options.allow_60fps != 0;
    }
    if (!win::SaveConfig(config)) {
        PrintError("saving the configuration failed\n");
        return 1;
    }

    // 3. Cameras.
    int failures = 0;
    if (upgrade && previous.backend != backend) {
        // Switching backends: take the other one down so apps do not list each camera twice.
        if (previous.backend == win::Backend::MediaFoundation) {
            for (int i = 0; i < previous.cameras && mf_supported; i++) {
                RemoveCamera(api, i, previous.names[i]);
            }
        } else {
            std::string ignored;
            RunRegsvr32(dir, true, &ignored);
            RemoveDirectShowKeys();
        }
    }
    if (backend == win::Backend::MediaFoundation) {
        // COM registration of the media source (HKLM: Frame Server reads it).
        const LSTATUS reg = win::RegisterSourceClasses(dir + L"\\vcamsource.dll");
        if (reg != ERROR_SUCCESS) {
            PrintError("registering the media source failed: %s\n", ErrorText(static_cast<DWORD>(reg)).c_str());
            return 1;
        }
        // Renamed or surplus cameras from an earlier install go first.
        if (previous.backend == win::Backend::MediaFoundation) {
            for (int i = 0; i < previous.cameras; i++) {
                if (i >= config.cameras || previous.names[i] != config.names[i]) {
                    RemoveCamera(api, i, previous.names[i]);
                }
            }
        }
        for (int i = 0; i < config.cameras; i++) {
            const HRESULT hr = RegisterCamera(api, i, config.names[i]);
            if (FAILED(hr)) {
                PrintError("registering camera %d (\"%s\") failed: %s\n", i + 1, Utf8(config.names[i]).c_str(),
                           HresultText(hr).c_str());
                if (hr == E_ACCESSDENIED) {
                    PrintError("  check Settings > Privacy & security > Camera: camera access must be on\n");
                }
                failures++;
            } else {
                Print("  camera %d: \"%s\"\n", i + 1, Utf8(config.names[i]).c_str());
            }
        }
    } else {
        std::string error;
        if (!RunRegsvr32(dir, false, &error)) {
            PrintError("%s\n", error.c_str());
            failures++;
        }
        for (int i = 0; i < config.cameras; i++) {
            const bool ok = DirectShowRegistered(i, false);
            Print("  camera %d: \"%s\"%s\n", i + 1, Utf8(config.names[i]).c_str(),
                  ok ? (DirectShowRegistered(i, true) ? "" : " (64-bit apps only)") : " NOT registered");
            failures += ok ? 0 : 1;
        }
    }

    // 4. Integration.
    if (!options.managed) {
        WriteUninstallEntry(dir);
        if (FileExists(dir + L"\\vcam-gui.exe")) {
            CreateStartMenuShortcut(dir + L"\\vcam-gui.exe");
        }
    }
    if (options.add_to_path) {
        UpdateMachinePath(dir, true);
    }

    Print("Modes: up to %s at 30%s fps, %s\n", FormatResolution(config.max_width, config.max_height).c_str(),
          config.allow_60fps ? " and 60" : "",
          backend == win::Backend::MediaFoundation ? "NV12 / YUY2 / RGB32" : "NV12 / YUY2 / I420 / RGB32");
    if (deferred) {
        Print("An older version was still in use; its files are removed at the next restart.\n");
    }
    if (failures) {
        return 1;
    }
    Print("Done. Apps now list the camera%s; feed %s with VCam or e.g. `vcam bars`.\n", config.cameras > 1 ? "s" : "",
          config.cameras > 1 ? "them" : "it");
    if (backend == win::Backend::DirectShow) {
        Print("(DirectShow: desktop apps such as Zoom, Teams, browsers and OBS see it; the Windows Camera app does "
              "not.)\n");
    }
    return 0;
}

int CmdUninstall(bool managed)
{
    if (!RequireElevation("vcam uninstall")) {
        return 1;
    }
    MediaFoundation mf;
    VirtualCameraApi api;
    const win::Config config = win::LoadConfig();
    const std::wstring dir = config.install_dir.empty() ? ProgramFilesDir() + L"\\vcam" : config.install_dir;
    if (config.backend == win::Backend::MediaFoundation && api.available()) {
        for (int i = 0; i < std::max(config.cameras, 1); i++) {
            const HRESULT hr = RemoveCamera(api, i, config.names[i]);
            Print("  camera %d (\"%s\"): %s\n", i + 1, Utf8(config.names[i]).c_str(),
                  SUCCEEDED(hr) ? "removed" : HresultText(hr).c_str());
        }
    }
    win::UnregisterSourceClasses();
    std::string ignored;
    RunRegsvr32(dir, true, &ignored);
    RemoveDirectShowKeys();

    // Remove the scheduled tasks created by `vcam autostart` and the GUI, if any.
    for (const wchar_t* task : {L"vcam", L"vcam-gui"}) {
        RunHidden(std::wstring(L"schtasks.exe /Delete /F /TN ") + task, 10000);
    }

    UpdateMachinePath(dir, false);
    HKEY software = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &software) == ERROR_SUCCESS) {
        RegDeleteTreeW(software, L"vcam");
        RegDeleteKeyExW(software, L"vcam", KEY_WOW64_64KEY, 0);
        RegCloseKey(software);
    }
    if (managed) {
        // The setup program removes the files and its Installed apps entry.
        Print("vcam's cameras and registrations are removed.\n");
        return 0;
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);
    const std::wstring shortcut = StartMenuShortcut();
    if (!shortcut.empty()) {
        DeleteFileW(shortcut.c_str());
    }

    // Files: anything in use (the camera inside Frame Server or an app, this
    // program) goes at the next restart.
    bool deferred = false;
    static const wchar_t* const files[] = {L"vcamsource.dll", L"vcamdshow.dll", L"vcamdshow32.dll", L"vcam.dll",
                                           L"sdk\\vcam.h", L"sdk\\vcam.lib", L"python\\vcam.py", L"python\\example.py",
                                           L"README.md", L"vcam-gui.exe", L"vcam.exe"};
    const bool running_from_install = SamePath(Directory(ExecutablePath()), dir);
    for (const wchar_t* f : files) {
        const std::wstring path = dir + L"\\" + f;
        if (FileExists(path) && !DeleteFileW(path.c_str())) {
            MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            deferred = true;
        }
    }
    for (const wchar_t* sub : {L"\\sdk", L"\\python", L""}) {
        const std::wstring d = dir + sub;
        if (!RemoveDirectoryW(d.c_str()) && GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) {
            MoveFileExW(d.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    if (running_from_install) {
        deferred = true;  // vcam.exe itself
    }
    Print("vcam is uninstalled.%s\n", deferred ? " Some files are in use and are removed at the next restart." : "");
    Print("Settings and logs in %s were kept; delete the folder to remove them.\n", Utf8(win::DataDirectory()).c_str());
    return 0;
}

int CmdStatus()
{
    MediaFoundation mf;
    VirtualCameraApi api;
    const win::Config config = win::LoadConfig();
    Print("vcam %s\n", VCAM_VERSION_STRING);
    Print("  Windows build %lu: virtual cameras %s\n", WindowsBuild(),
          api.available() ? "supported" : "NOT supported (Windows 11 is required)");
    if (config.install_dir.empty()) {
        Print("  Not installed. Run `vcam install` as administrator.\n");
        return 1;
    }
    const bool dshow = config.backend == win::Backend::DirectShow;
    std::wstring server;
    const std::wstring clsid_key = L"SOFTWARE\\Classes\\CLSID\\" +
                                   win::GuidString(dshow ? win::DirectShowClsid(0) : win::SourceClsid(0)) +
                                   L"\\InprocServer32";
    const bool registered = win::RegReadString(HKEY_LOCAL_MACHINE, clsid_key.c_str(), nullptr, &server);
    Print("  Installed in %s\n", Utf8(config.install_dir).c_str());
    Print("  Backend: %s\n", dshow ? "DirectShow (desktop apps; not the Windows Camera app)"
                                    : "Media Foundation (all apps, through Windows Frame Server)");
    Print("  %s: %s\n", dshow ? "Filter" : "Media source",
          !registered          ? "NOT registered (run `vcam install`)"
          : FileExists(server) ? Utf8(server).c_str()
                               : "registered, but the file is missing (run `vcam install`)");
    Print("  Modes: up to %s, %s\n", FormatResolution(config.max_width, config.max_height).c_str(),
          config.allow_60fps ? "30 and 60 fps" : "30 fps");
    Print("  Placeholder: %s\n", config.placeholder.empty() ? "built in" : Utf8(config.placeholder).c_str());
    Print("  Logging: %s\n", config.logging ? Utf8(win::DataDirectory() + L"\\logs").c_str() : "off");

    const std::vector<RegisteredCamera> devices = dshow ? std::vector<RegisteredCamera>() : VideoDevices(config);
    const int64_t now = win::NowTicks();
    int problems = registered ? 0 : 1;
    for (int i = 0; i < config.cameras; i++) {
        if (dshow) {
            const bool x64 = DirectShowRegistered(i, false), x86 = DirectShowRegistered(i, true);
            Print("\nCamera %d \"%s\": %s\n", i + 1, Utf8(config.names[i]).c_str(),
                  !x64 ? "NOT registered with DirectShow (run `vcam install`)"
                  : x86 ? "visible to applications (64-bit and 32-bit)"
                        : "visible to 64-bit applications");
            problems += x64 ? 0 : 1;
        } else {
            const auto it =
                std::find_if(devices.begin(), devices.end(), [&](const RegisteredCamera& d) { return d.slot == i; });
            Print("\nCamera %d \"%s\": %s\n", i + 1, Utf8(config.names[i]).c_str(),
                  it != devices.end() ? "visible to applications" : "NOT registered with Windows (run `vcam install`)");
            if (it == devices.end()) {
                problems++;
            } else if (it->name != config.names[i]) {
                Print("  Listed by apps as \"%s\"\n", Utf8(it->name).c_str());
            }
        }
        win::SharedFrames shm;
        if (!shm.Open(i, false, 0, dshow)) {
            Print("  Idle: no app is using it and no producer is running.\n");
            continue;
        }
        const shm::Header* h = shm.ring().header();
        const bool producing = ProducerAlive(h, now);
        Print("  Producer: %s\n", producing ? ProcessName(AtomicLoad(h->producer_pid)).c_str() : "none (apps see the placeholder)");
        shm::FrameRequest request;
        if (ConsumerStreaming(h, now) && shm.ring().ReadRequest(&request) && IsValidFormat(request.format)) {
            Print("  Apps: streaming %s %s at %.4g fps\n", FormatResolution(request.width, request.height).c_str(),
                  FormatName(static_cast<PixelFormat>(request.format)),
                  request.fps_den ? static_cast<double>(request.fps_num) / request.fps_den : 0.0);
        } else {
            Print("  Apps: none streaming\n");
        }
        Print("  Frames: %llu written, %llu delivered, %llu repeats\n",
              static_cast<unsigned long long>(AtomicLoad(h->frames_written, std::memory_order_relaxed)),
              static_cast<unsigned long long>(AtomicLoad(h->frames_delivered, std::memory_order_relaxed)),
              static_cast<unsigned long long>(AtomicLoad(h->frames_repeated, std::memory_order_relaxed)));
    }
    return problems ? 1 : 0;
}

int CmdConfig(Args& args)
{
    win::Config config = win::LoadConfig();
    if (config.install_dir.empty()) {
        PrintError("vcam is not installed\n");
        return 1;
    }
    std::string v;
    bool changed = false, renamed = false;
    win::Config previous = config;
    while (args.Value("name", &v)) {
        int index = 1;
        std::string name = v;
        const size_t eq = v.find('=');
        if (eq != std::string::npos && ParseInt(v.substr(0, eq), &index)) {
            name = v.substr(eq + 1);
        }
        if (index < 1 || index > config.cameras || name.empty()) {
            PrintError("--name [N=]NAME: N must be a camera number from 1 to %d\n", config.cameras);
            return 2;
        }
        config.names[index - 1] = Wide(name);
        changed = renamed = true;
    }
    if (args.Value("max-resolution", &v)) {
        if (!ParseResolution(v.c_str(), &config.max_width, &config.max_height)) {
            PrintError("bad resolution '%s' (1080p, 1440p, 4k or WxH)\n", v.c_str());
            return 2;
        }
        changed = true;
    }
    if (args.Value("60fps", &v)) {
        config.allow_60fps = v == "on" || v == "yes" || v == "1";
        changed = true;
    }
    if (args.Value("logging", &v)) {
        config.logging = v == "on" || v == "yes" || v == "1";
        changed = true;
    }
    if (!args.Finish()) {
        return 2;
    }
    if (!changed) {
        return CmdStatus();
    }
    if (!RequireElevation("vcam config")) {
        return 1;
    }
    if (config.logging) {
        CreateDataDirectories();
    }
    if (!win::SaveConfig(config)) {
        PrintError("saving the configuration failed\n");
        return 1;
    }
    if (renamed && config.backend == win::Backend::DirectShow) {
        std::string error;
        if (!RunRegsvr32(config.install_dir, false, &error)) {
            PrintError("%s\n", error.c_str());
        }
    } else if (renamed) {
        MediaFoundation mf;
        VirtualCameraApi api;
        for (int i = 0; i < config.cameras; i++) {
            if (previous.names[i] != config.names[i]) {
                RemoveCamera(api, i, previous.names[i]);
                const HRESULT hr = RegisterCamera(api, i, config.names[i]);
                Print("  camera %d is now \"%s\"%s\n", i + 1, Utf8(config.names[i]).c_str(),
                      SUCCEEDED(hr) ? "" : (": " + HresultText(hr)).c_str());
            }
        }
    }
    Print("Saved. It applies the next time an app opens a camera (`vcam restart-frame-server` applies it now).\n");
    return 0;
}

bool SetPlaceholderImage(const std::wstring& file, std::string* error)
{
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!LoadImageBgra(file, &pixels, &w, &h, error)) {
        return false;
    }
    // Store at most 1920x1080; the camera scales it to whatever it streams.
    const Placement fit = ComputePlacement(w, h, std::min(w, 1920), std::min(h, 1080), ScaleMode::Fit, false);
    std::vector<uint8_t> stored(static_cast<size_t>(fit.dst_w) * fit.dst_h * 4);
    Converter converter;
    ConvertOptions options;
    options.scale = ScaleMode::Stretch;
    converter.Convert(MakeView(PixelFormat::BGRA, w, h, pixels.data()),
                      MakeSpan(PixelFormat::BGRA, fit.dst_w, fit.dst_h, stored.data()), options);
    CreateDataDirectories();
    const std::wstring path = win::DataDirectory() + L"\\placeholder.vcamimg";
    if (!win::SavePlaceholderImage(path, stored.data(), static_cast<uint32_t>(fit.dst_w),
                                   static_cast<uint32_t>(fit.dst_h))) {
        *error = "writing " + Utf8(path) + " failed: " + ErrorText(GetLastError());
        return false;
    }
    win::Config config = win::LoadConfig();
    config.placeholder = path;
    if (!win::SaveConfig(config)) {
        *error = "saving the configuration failed";
        return false;
    }
    return true;
}

bool ResetPlaceholderImage(std::string* error)
{
    win::Config config = win::LoadConfig();
    config.placeholder.clear();
    if (!win::SaveConfig(config)) {
        *error = "saving the configuration failed";
        return false;
    }
    return true;
}

int CmdPlaceholder(Args& args)
{
    const bool reset = args.Flag("reset");
    const std::string file = args.Next();
    if (!args.Finish() || (reset == !file.empty())) {
        PrintError("usage: vcam placeholder FILE | --reset\n");
        return 2;
    }
    if (!RequireElevation("vcam placeholder")) {
        return 1;
    }
    std::string error;
    if (!(reset ? ResetPlaceholderImage(&error) : SetPlaceholderImage(Wide(file), &error))) {
        PrintError("%s\n", error.c_str());
        return 1;
    }
    Print(reset ? "The built-in placeholder is back.\n" : "Placeholder set.\n");
    Print("It shows the next time an app opens a camera with no producer running.\n");
    return 0;
}

int CmdRestartFrameServer()
{
    if (!RequireElevation("vcam restart-frame-server")) {
        return 1;
    }
    // Frame Server starts again on demand; apps using any camera reconnect.
    const bool monitor = StopService(L"FrameServerMonitor");
    const bool server = StopService(L"FrameServer");
    Print("Frame Server %s; it restarts the next time an app opens a camera.\n",
          server || monitor ? "stopped" : "was not running");
    return 0;
}

int CmdAutostart(Args& args, int argc, wchar_t** argv)
{
    const bool off = args.Flag("off");
    if (!RequireElevation("vcam autostart")) {
        return 1;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command;
    if (off) {
        if (!args.Finish()) {
            return 2;
        }
        command = L"schtasks.exe /Delete /F /TN vcam";
    } else {
        // Everything after "autostart" is the source command line.
        std::wstring source_args;
        int first = 0;
        for (int i = 1; i < argc; i++) {
            if (Utf8(argv[i]) == "autostart") {
                first = i + 1;
                break;
            }
        }
        if (first == 0 || first >= argc) {
            PrintError("usage: vcam autostart SOURCE [ARGS...]   e.g. vcam autostart image C:\\me.png --fill\n");
            return 2;
        }
        for (int i = first; i < argc; i++) {
            std::wstring a = argv[i];
            if (a.find_first_of(L" \t\"") != std::wstring::npos) {
                std::wstring quoted;
                for (wchar_t c : a) {
                    quoted += c == L'"' ? std::wstring(L"\\\"") : std::wstring(1, c);
                }
                a = L"\\\"" + quoted + L"\\\"";  // escaped for schtasks /TR
            }
            source_args += L" " + a;
        }
        const win::Config config = win::LoadConfig();
        const std::wstring exe = (config.install_dir.empty() ? Directory(ExecutablePath()) : config.install_dir) + L"\\vcam.exe";
        command = L"schtasks.exe /Create /F /TN vcam /SC ONLOGON /RL HIGHEST /TR \"\\\"" + exe + L"\\\"" +
                  source_args + L" --hidden\"";
    }
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        PrintError("running schtasks failed: %s\n", ErrorText(GetLastError()).c_str());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) {
        return 1;
    }
    Print(off ? "Autostart removed.\n" : "The source now starts when you sign in (scheduled task \"vcam\").\n");
    return 0;
}

void EnsureCamerasRegistered()
{
    if (!IsElevated()) {
        return;
    }
    const win::Config config = win::LoadConfig();
    if (config.cameras == 0) {
        return;
    }
    if (config.backend == win::Backend::DirectShow) {
        for (int i = 0; i < config.cameras; i++) {
            std::string error;
            if (!DirectShowRegistered(i, false) && RunRegsvr32(config.install_dir, false, &error)) {
                Print("The cameras were missing from DirectShow and have been registered again.\n");
                break;
            }
        }
        return;
    }
    MediaFoundation mf;
    VirtualCameraApi api;
    if (!api.available()) {
        return;
    }
    const std::vector<RegisteredCamera> devices = VideoDevices(config);
    for (int i = 0; i < config.cameras; i++) {
        const bool found =
            std::any_of(devices.begin(), devices.end(), [&](const RegisteredCamera& d) { return d.slot == i; });
        if (!found && SUCCEEDED(RegisterCamera(api, i, config.names[i]))) {
            Print("Camera %d was missing from Windows and has been registered again.\n", i + 1);
        }
    }
}

std::vector<CameraInfo> EnumerateCameras()
{
    MediaFoundation mf;
    std::vector<CameraInfo> cameras;
    const win::Config config = win::LoadConfig();
    for (const RegisteredCamera& d : VideoDevices(config)) {
        CameraInfo info;
        info.name = d.name;
        info.symbolic_link = d.link;
        info.is_vcam = d.slot >= 0 || Lower(d.link).find(L"vcamdevapi") != std::wstring::npos;
        cameras.push_back(info);
    }
    return cameras;
}

}  // namespace vcam::app
