// VCam, the GUI: a window hosting the interface (WebView2, windows/gui/ui) that
// installs and configures vcam and feeds the cameras from the built-in sources.
//
// Threads: the UI thread owns the window, WebView2 and everything the page
// sees. A worker thread runs slow jobs (starting and stopping sources,
// installing, listing devices) and posts results back. Each running camera
// has its source runner thread, which also feeds the preview.
#pragma once

#include "app.h"
#include "json.h"

#include <condition_variable>
#include <deque>
#include <functional>

namespace vcam::gui {

constexpr UINT WM_APP_POST = WM_APP + 1;   // run callbacks queued for the UI thread
constexpr UINT WM_APP_FRAME = WM_APP + 2;  // a preview frame is waiting
constexpr UINT WM_APP_TRAY = WM_APP + 3;   // notification area icon

// Runs jobs one after another on a thread of its own.
class Worker {
public:
    Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker();
    void Post(std::function<void()> job);

private:
    void Run();
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false;
    std::thread thread_;
};

// The preview: frames of the selected camera, converted to the size the page
// shows them at (RGBA, what a canvas takes).
class Preview {
public:
    void Select(int camera);
    int selected() const { return selected_; }
    void SetSize(int width, int height);
    void Size(int* width, int* height);

    // Runner threads: a frame the camera just got. Converted at most 30 times
    // a second, and only for the selected camera.
    void OnSourceFrame(int camera, const app::Frame& frame, const app::SourceOptions& options, HWND notify);
    // UI thread: a frame read from the shared memory (another producer) or the placeholder.
    void SetFrame(int camera, std::vector<uint8_t> rgba, int width, int height);

    // UI thread: takes the newest frame if there is one.
    bool Take(int camera, std::vector<uint8_t>* rgba, int* width, int* height);

private:
    std::atomic<int> selected_{0};
    std::mutex lock_;  // everything below
    int width_ = 640, height_ = 360;
    std::vector<uint8_t> pending_;
    int pending_camera_ = -1, pending_w_ = 0, pending_h_ = 0;
    bool fresh_ = false;
    bool notified_ = false;
    int64_t last_[win::kMaxCameras] = {};
    Converter converters_[win::kMaxCameras];  // each used by its camera's runner thread only
};

// --- platform helpers (platform.cpp) ---------------------------------------------

bool SystemUsesDarkTheme();
// The system accent colour for light and dark themes, "#rrggbb" ("" if unknown).
void AccentColors(std::string* for_light, std::string* for_dark);
bool MicaSupported();
void ApplyWindowTheme(HWND window, bool mica);

// Modal file picker; kind is "image", "video" or "placeholder". Empty if cancelled.
std::wstring PickFile(HWND owner, const std::string& kind);
// A small RGBA thumbnail as {w, h, data: base64}, or Null.
Json ImageThumbnail(const std::wstring& path, int width, int height);
Json PlaceholderThumbnail(int width, int height);
// The picture apps see with no source: the configured placeholder or the built-in one.
void RenderPlaceholderRgba(int width, int height, std::vector<uint8_t>* rgba);

std::string ProcessName(DWORD pid);
std::wstring ExecutablePath();

// Scheduled task that starts the GUI in the background at sign-in.
bool AutostartEnabled();
bool SetAutostart(bool enable, std::string* error);

// HKCU\Software\vcam\gui
std::string LoadUiState();
void SaveUiState(const std::string& json);
bool LoadTraySetting();
void SaveTraySetting(bool tray);
bool LoadPlacement(WINDOWPLACEMENT* placement);
void SavePlacement(const WINDOWPLACEMENT& placement);

}  // namespace vcam::gui
