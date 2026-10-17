#include "common.h"

#include <objbase.h>
#include <sddl.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace vcam::win {

// --- identities ------------------------------------------------------------------

GUID SourceClsid(int index)
{
    return GUID{0xb3ec4cda, 0x11f2, 0x4d3d, {0x84, 0x71, 0x47, 0x63, 0xb6, 0xa6, 0xc8, static_cast<unsigned char>(0x20 + index)}};
}

bool CameraFromClsid(REFCLSID clsid, int* index)
{
    for (int i = 0; i < kMaxCameras; i++) {
        if (IsEqualGUID(clsid, SourceClsid(i))) {
            *index = i;
            return true;
        }
    }
    return false;
}

GUID DirectShowClsid(int index)
{
    return GUID{0xb3ec4cda, 0x11f2, 0x4d3d, {0x84, 0x71, 0x47, 0x63, 0xb6, 0xa6, 0xc8, static_cast<unsigned char>(0x30 + index)}};
}

bool CameraFromDirectShowClsid(REFCLSID clsid, int* index)
{
    for (int i = 0; i < kMaxCameras; i++) {
        if (IsEqualGUID(clsid, DirectShowClsid(i))) {
            *index = i;
            return true;
        }
    }
    return false;
}

std::wstring GuidString(REFGUID guid)
{
    wchar_t text[64] = {};
    StringFromGUID2(guid, text, 64);
    return text;
}

namespace {

std::wstring ObjectName(int index, const wchar_t* suffix, bool local = false)
{
    return (local ? L"Local\\VCam.v1.Camera" : L"Global\\VCam.v1.Camera") + std::to_wstring(index) + suffix;
}

}  // namespace

std::wstring ShmName(int index, bool local)
{
    return ObjectName(index, L".Frames", local);
}

std::wstring FrameEventName(int index)
{
    return ObjectName(index, L".FrameReady");
}

std::wstring RequestEventName(int index)
{
    return ObjectName(index, L".RequestChanged");
}

std::wstring ProducerMutexName(int index)
{
    return ObjectName(index, L".Producer");
}

// --- registry ---------------------------------------------------------------------

bool RegReadString(HKEY root, const wchar_t* path, const wchar_t* name, std::wstring* value)
{
    DWORD bytes = 0;
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr,
                     &bytes) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring text(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                     text.data(), &bytes) != ERROR_SUCCESS) {
        return false;
    }
    text.resize(wcslen(text.c_str()));
    *value = text;
    return true;
}

bool RegReadDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD* value)
{
    DWORD bytes = sizeof(*value);
    return RegGetValueW(root, path, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes) ==
           ERROR_SUCCESS;
}

LSTATUS RegWriteString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status;
}

LSTATUS RegWriteDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD value)
{
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status;
}

LSTATUS RegisterSourceClasses(const std::wstring& dll_path)
{
    for (int i = 0; i < kMaxCameras; i++) {
        const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + GuidString(SourceClsid(i));
        LSTATUS status = RegWriteString(HKEY_LOCAL_MACHINE, key.c_str(), nullptr,
                                        L"VCam media source " + std::to_wstring(i + 1));
        if (status == ERROR_SUCCESS) {
            status = RegWriteString(HKEY_LOCAL_MACHINE, (key + L"\\InprocServer32").c_str(), nullptr, dll_path);
        }
        if (status == ERROR_SUCCESS) {
            status = RegWriteString(HKEY_LOCAL_MACHINE, (key + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Both");
        }
        if (status != ERROR_SUCCESS) {
            return status;
        }
    }
    return ERROR_SUCCESS;
}

void UnregisterSourceClasses()
{
    for (int i = 0; i < kMaxCameras; i++) {
        HKEY classes = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                          &classes) == ERROR_SUCCESS) {
            RegDeleteTreeW(classes, GuidString(SourceClsid(i)).c_str());
            RegDeleteKeyExW(classes, GuidString(SourceClsid(i)).c_str(), KEY_WOW64_64KEY, 0);
            RegCloseKey(classes);
        }
    }
}

// --- configuration -------------------------------------------------------------------

const char* BackendName(Backend backend)
{
    return backend == Backend::DirectShow ? "DirectShow" : "Media Foundation";
}

std::wstring DefaultCameraName(int index)
{
    return index == 0 ? L"VCam" : L"VCam " + std::to_wstring(index + 1);
}

Config LoadConfig()
{
    Config c;
    DWORD value = 0;
    RegReadString(HKEY_LOCAL_MACHINE, kRegistryKey, L"InstallDir", &c.install_dir);
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Cameras", &value)) {
        c.cameras = static_cast<int>(value > kMaxCameras ? kMaxCameras : value);
    }
    for (int i = 0; i < kMaxCameras; i++) {
        const std::wstring name = L"Name" + std::to_wstring(i);
        if (!RegReadString(HKEY_LOCAL_MACHINE, kRegistryKey, name.c_str(), &c.names[i]) || c.names[i].empty()) {
            c.names[i] = DefaultCameraName(i);
        }
    }
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"MaxWidth", &value) && value >= 320 && value <= 4096) {
        c.max_width = value;
    }
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"MaxHeight", &value) && value >= 180 && value <= 4096) {
        c.max_height = value;
    }
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Allow60fps", &value)) {
        c.allow_60fps = value != 0;
    }
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Logging", &value)) {
        c.logging = value != 0;
    }
    RegReadString(HKEY_LOCAL_MACHINE, kRegistryKey, L"Placeholder", &c.placeholder);
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Backend", &value) &&
        value == static_cast<DWORD>(Backend::DirectShow)) {
        c.backend = Backend::DirectShow;
    }
    if (RegReadDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Managed", &value)) {
        c.managed = value != 0;
    }
    return c;
}

bool SaveConfig(const Config& c)
{
    bool ok = RegWriteString(HKEY_LOCAL_MACHINE, kRegistryKey, L"InstallDir", c.install_dir) == ERROR_SUCCESS;
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Cameras", static_cast<DWORD>(c.cameras)) == ERROR_SUCCESS;
    for (int i = 0; i < kMaxCameras; i++) {
        ok &= RegWriteString(HKEY_LOCAL_MACHINE, kRegistryKey, (L"Name" + std::to_wstring(i)).c_str(), c.names[i]) ==
              ERROR_SUCCESS;
    }
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"MaxWidth", c.max_width) == ERROR_SUCCESS;
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"MaxHeight", c.max_height) == ERROR_SUCCESS;
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Allow60fps", c.allow_60fps ? 1 : 0) == ERROR_SUCCESS;
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Logging", c.logging ? 1 : 0) == ERROR_SUCCESS;
    ok &= RegWriteString(HKEY_LOCAL_MACHINE, kRegistryKey, L"Placeholder", c.placeholder) == ERROR_SUCCESS;
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Backend", static_cast<DWORD>(c.backend)) == ERROR_SUCCESS;
    ok &= RegWriteDword(HKEY_LOCAL_MACHINE, kRegistryKey, L"Managed", c.managed ? 1 : 0) == ERROR_SUCCESS;
    return ok;
}

std::wstring DataDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buffer) : std::wstring(L"C:\\ProgramData");
    return base + L"\\vcam";
}

// --- clock -----------------------------------------------------------------------------

int64_t NowTicks()
{
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const int64_t whole = counter.QuadPart / frequency;
    const int64_t rest = counter.QuadPart % frequency;
    return whole * 10'000'000 + rest * 10'000'000 / frequency;
}

// --- logging ---------------------------------------------------------------------------

namespace {

std::mutex g_log_lock;
HANDLE g_log_file = INVALID_HANDLE_VALUE;
std::string g_component = "vcam";

}  // namespace

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

void LogInit(const wchar_t* component, bool per_user)
{
    std::lock_guard<std::mutex> lock(g_log_lock);
    g_component = Narrow(component);
    if (g_log_file != INVALID_HANDLE_VALUE || !LoadConfig().logging) {
        return;
    }
    std::wstring dir = DataDirectory() + L"\\logs";
    if (per_user) {
        // Inside an application: the user's own folder, never a shared one.
        wchar_t local[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return;
        }
        dir = std::wstring(local) + L"\\vcam";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    const std::wstring path = dir + L"\\" + component + L".log";
    g_log_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(g_log_file, &size) && size.QuadPart > (16 << 20)) {
            CloseHandle(g_log_file);  // keep logs bounded; delete the file to start over
            g_log_file = INVALID_HANDLE_VALUE;
        }
    }
}

void Log(const char* format, ...)
{
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    std::lock_guard<std::mutex> lock(g_log_lock);
    const int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s %lu:%lu] %s\r\n", t.wYear,
                           t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, g_component.c_str(),
                           GetCurrentProcessId(), GetCurrentThreadId(), message);
    OutputDebugStringA(line);
    if (g_log_file != INVALID_HANDLE_VALUE && n > 0) {
        DWORD written = 0;
        WriteFile(g_log_file, line, static_cast<DWORD>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1),
                  &written, nullptr);
    }
}

// --- shared memory -----------------------------------------------------------------------

namespace {

// Everyone who logs on may produce or consume; the media source runs as LOCAL
// SERVICE. The medium integrity label lets non-elevated producers write to
// objects created by elevated ones.
constexpr wchar_t kSectionSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGW;;;AU)S:(ML;;NW;;;ME)";
constexpr wchar_t kEventSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGWGX;;;AU)S:(ML;;NW;;;ME)";

class SecurityAttributes {
public:
    explicit SecurityAttributes(const wchar_t* sddl)
    {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor_, nullptr)) {
            attributes_.nLength = sizeof(attributes_);
            attributes_.lpSecurityDescriptor = descriptor_;
            attributes_.bInheritHandle = FALSE;
        }
    }
    ~SecurityAttributes()
    {
        if (descriptor_) {
            LocalFree(descriptor_);
        }
    }
    SECURITY_ATTRIBUTES* get() { return descriptor_ ? &attributes_ : nullptr; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

}  // namespace

HANDLE OpenOrCreateSharedEvent(const std::wstring& name)
{
    SecurityAttributes sa(kEventSddl);
    HANDLE h = CreateEventW(sa.get(), FALSE, FALSE, name.c_str());
    if (!h) {
        // Exists with a DACL that does not grant us EVENT_ALL_ACCESS.
        h = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name.c_str());
    }
    return h;
}

HANDLE OpenOrCreateSharedMutex(const std::wstring& name)
{
    SecurityAttributes sa(kEventSddl);
    HANDLE h = CreateMutexW(sa.get(), FALSE, name.c_str());
    if (!h) {
        h = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
    }
    return h;
}

SharedFrames::~SharedFrames()
{
    Close();
}

bool SharedFrames::Open(int index, bool create, uint64_t capacity, bool allow_local)
{
    if (is_open()) {
        return true;
    }
    Close();
    bool local = false;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ShmName(index).c_str());
    if (!mapping && allow_local) {
        mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ShmName(index, true).c_str());
        local = mapping != nullptr;
    }
    bool created = false;
    if (!mapping && create) {
        const uint64_t size = ShmBytesFor(capacity);
        SecurityAttributes sa(kSectionSddl);
        for (const bool in_local : {false, true}) {
            if (in_local && !allow_local) {
                break;
            }
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.get(), PAGE_READWRITE | SEC_COMMIT,
                                         static_cast<DWORD>(size >> 32), static_cast<DWORD>(size),
                                         ShmName(index, in_local).c_str());
            if (mapping) {
                created = GetLastError() != ERROR_ALREADY_EXISTS;
                local = in_local;
                break;
            }
        }
    }
    if (!mapping) {
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION info{};
    if (!view || !VirtualQuery(view, &info, sizeof(info))) {
        if (view) {
            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
        return false;
    }
    if (created) {
        InitializeShm(view, info.RegionSize, capacity);
    }
    // A mapping someone else just created may not be initialised yet; the
    // caller retries later in that case.
    if (!ring_.Attach(view, info.RegionSize)) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }
    frame_event_ = OpenOrCreateSharedEvent(FrameEventName(index));
    request_event_ = OpenOrCreateSharedEvent(RequestEventName(index));
    mapping_ = mapping;
    view_ = view;
    created_ = created;
    local_ = local;
    return true;
}

void SharedFrames::Close()
{
    ring_.Detach();
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    for (HANDLE* h : {&mapping_, &frame_event_, &request_event_}) {
        if (*h) {
            CloseHandle(*h);
            *h = nullptr;
        }
    }
    created_ = false;
    local_ = false;
}

// --- placeholder images ----------------------------------------------------------------------

bool SavePlaceholderImage(const std::wstring& path, const uint8_t* bgra, uint32_t width, uint32_t height)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    const uint32_t size[2] = {width, height};
    DWORD written = 0;
    bool ok = WriteFile(f, "VCAMIMG1", 8, &written, nullptr) && written == 8;
    ok = ok && WriteFile(f, size, 8, &written, nullptr) && written == 8;
    const DWORD bytes = width * height * 4;
    ok = ok && WriteFile(f, bgra, bytes, &written, nullptr) && written == bytes;
    CloseHandle(f);
    return ok;
}

bool LoadPlaceholderImage(const std::wstring& path, std::string* pixels, uint32_t* width, uint32_t* height)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    char magic[8] = {};
    uint32_t size[2] = {};
    DWORD got = 0;
    bool ok = ReadFile(f, magic, 8, &got, nullptr) && got == 8 && memcmp(magic, "VCAMIMG1", 8) == 0 &&
              ReadFile(f, size, 8, &got, nullptr) && got == 8 && size[0] >= 1 && size[1] >= 1 && size[0] <= 4096 &&
              size[1] <= 4096;
    if (ok) {
        pixels->resize(static_cast<size_t>(size[0]) * size[1] * 4);
        ok = ReadFile(f, pixels->data(), static_cast<DWORD>(pixels->size()), &got, nullptr) && got == pixels->size();
        *width = size[0];
        *height = size[1];
    }
    CloseHandle(f);
    return ok;
}

}  // namespace vcam::win
