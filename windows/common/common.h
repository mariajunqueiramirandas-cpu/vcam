// Windows pieces shared by the media source, the producer SDK and the vcam tool:
// names of shared objects, configuration, the clock, logging and the shared
// memory transport.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

#include "vcam/ring.h"

namespace vcam::win {

constexpr int kMaxCameras = 4;

// --- identities ----------------------------------------------------------------

// CLSID of the media source for camera `index` (0-based):
// {B3EC4CDA-11F2-4D3D-8471-4763B6A6C820} with the index added to the last byte.
GUID SourceClsid(int index);
bool CameraFromClsid(REFCLSID clsid, int* index);
// CLSID of the DirectShow filter for camera `index`: ...C830 plus the index.
GUID DirectShowClsid(int index);
bool CameraFromDirectShowClsid(REFCLSID clsid, int* index);
std::wstring GuidString(REFGUID guid);

// Kernel object names. Global\ so the media source (Frame Server, session 0)
// and producers in any user session see the same objects. With the DirectShow
// backend the mapping may also live in the session's Local\ namespace, see
// SharedFrames::Open.
std::wstring ShmName(int index, bool local = false);
std::wstring FrameEventName(int index);
std::wstring RequestEventName(int index);
std::wstring ProducerMutexName(int index);

// --- configuration (HKLM\SOFTWARE\vcam) -----------------------------------------

constexpr wchar_t kRegistryKey[] = L"SOFTWARE\\vcam";

// How the cameras reach applications.
enum class Backend : uint32_t {
    // Windows 11: a Media Foundation virtual camera served by Frame Server, seen
    // by every app (Media Foundation, WinRT and DirectShow).
    MediaFoundation = 0,
    // Windows 10: a DirectShow capture filter loaded into each app, seen by
    // DirectShow apps (Zoom, Teams, browsers, OBS...).
    DirectShow = 1,
};
const char* BackendName(Backend backend);

struct Config {
    std::wstring install_dir;
    int cameras = 0;                      // cameras registered by `vcam install`
    std::wstring names[kMaxCameras];      // friendly names
    uint32_t max_width = 1920;            // largest mode advertised
    uint32_t max_height = 1080;
    bool allow_60fps = true;
    bool logging = false;                 // write logs to %ProgramData%\vcam\logs
    std::wstring placeholder;             // image shown with no producer (.vcamimg), empty = built in
    Backend backend = Backend::MediaFoundation;
};

Config LoadConfig();
bool SaveConfig(const Config& config);  // needs administrator rights
std::wstring DefaultCameraName(int index);

// %ProgramData%\vcam (created by `vcam install`, readable by everyone).
std::wstring DataDirectory();

// --- registry helpers ------------------------------------------------------------

bool RegReadString(HKEY root, const wchar_t* path, const wchar_t* name, std::wstring* value);
bool RegReadDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD* value);
LSTATUS RegWriteString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value);
LSTATUS RegWriteDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD value);

// Registers / removes the media source COM classes (HKLM, all camera slots).
LSTATUS RegisterSourceClasses(const std::wstring& dll_path);
void UnregisterSourceClasses();

// --- clock -------------------------------------------------------------------------

// QueryPerformanceCounter in 100 ns units: the clock Media Foundation timestamps
// (MFGetSystemTime) use, and what the shared memory's timestamps are in.
int64_t NowTicks();

// --- logging -----------------------------------------------------------------------

// Messages go to OutputDebugString, and to <component>.log when logging is
// enabled in the configuration: in %ProgramData%\vcam\logs, or, for code that
// runs inside applications (`per_user`), in %LOCALAPPDATA%\vcam\logs.
void LogInit(const wchar_t* component, bool per_user = false);
void Log(const char* format, ...);
std::string Narrow(const std::wstring& text);
std::wstring Widen(const std::string& text);

// --- shared memory -----------------------------------------------------------------

// The frame ring for one camera plus its two events.
class SharedFrames {
public:
    SharedFrames() = default;
    SharedFrames(const SharedFrames&) = delete;
    SharedFrames& operator=(const SharedFrames&) = delete;
    ~SharedFrames();

    // Opens camera `index`'s mapping. When it does not exist yet and `create`
    // is set, creates it with room for `capacity` bytes per slot. Creating a
    // Global\ mapping needs SeCreateGlobalPrivilege (administrators, services)
    // or running in session 0; otherwise only opening works.
    //
    // `allow_local` (DirectShow backend, where producer and consumers usually
    // run in the same user session without that privilege): look for the
    // mapping in Global\ and then Local\, and create it in Global\ if
    // allowed, otherwise in Local\. Both sides follow the same order, so they
    // meet in one namespace.
    // Returns true once attached to an initialised ring.
    bool Open(int index, bool create, uint64_t capacity, bool allow_local = false);
    void Close();

    bool is_open() const { return ring_.attached(); }
    bool created() const { return created_; }
    bool local() const { return local_; }  // attached to the session's Local\ mapping
    Ring& ring() { return ring_; }
    const Ring& ring() const { return ring_; }
    HANDLE frame_event() const { return frame_event_; }      // set by the producer after each frame
    HANDLE request_event() const { return request_event_; }  // set by the consumer when its request changes

private:
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    HANDLE frame_event_ = nullptr;
    HANDLE request_event_ = nullptr;
    Ring ring_;
    bool created_ = false;
    bool local_ = false;
};

// Opens or creates a Global\ mutex or event with the same access rules as the
// shared memory (anyone who logs on may use it).
HANDLE OpenOrCreateSharedMutex(const std::wstring& name);
HANDLE OpenOrCreateSharedEvent(const std::wstring& name);

// Placeholder images: "VCAMIMG1", uint32 width, uint32 height, BGRA pixels.
bool SavePlaceholderImage(const std::wstring& path, const uint8_t* bgra, uint32_t width, uint32_t height);
bool LoadPlaceholderImage(const std::wstring& path, std::string* pixels, uint32_t* width, uint32_t* height);

}  // namespace vcam::win
