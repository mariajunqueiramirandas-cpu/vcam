// The vcam tool library, shared by the command-line tool (vcam.exe) and the
// GUI (VCam.exe): installs and manages the cameras, and feeds them from
// built-in sources (test pattern, colour, image, video, webcam, screen,
// window).
#pragma once

#include "common.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <future>

#include "vcam.h"
#include "vcam/convert.h"

namespace vcam::app {

// --- output -------------------------------------------------------------------------

// Messages go to the console by default; the GUI installs its own sink.
void Print(const char* format, ...);
void PrintError(const char* format, ...);
using OutputSink = void (*)(bool error, const std::string& text);
void SetOutputSink(OutputSink sink);
std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);
std::string ErrorText(DWORD error);
std::string HresultText(HRESULT hr);

// Set by Ctrl+C / closing the console.
extern std::atomic<bool> g_stop;
void InstallStopHandler();

bool IsElevated();
bool RequireElevation(const char* what);

// --- command line -------------------------------------------------------------------

class Args {
public:
    Args(int argc, wchar_t** argv);
    bool empty() const { return pos_ >= items_.size(); }
    std::string Next();                        // next positional argument ("" if none)
    bool Flag(const char* name);               // removes --name if present
    bool Value(const char* name, std::string* value);  // removes --name VALUE / --name=VALUE
    // Everything that was not consumed; reports leftovers as errors.
    bool Finish();

private:
    std::vector<std::string> items_;
    std::vector<bool> used_;
    size_t pos_ = 0;
};

bool ParseColor(const std::string& text, Rgb* color);
bool ParseInt(const std::string& text, int* value);

// --- system management (system.cpp) -------------------------------------------------

struct InstallOptions {
    int cameras = 1;
    std::vector<std::wstring> names;  // per camera; empty = keep / default
    uint32_t max_width = 0, max_height = 0;  // 0 = keep / default
    int allow_60fps = -1;                    // -1 = keep
    bool add_to_path = true;
    // -1: Media Foundation where Windows supports it (Windows 11), DirectShow otherwise.
    int backend = -1;
    // Installed by the setup program: the files are already in place and it
    // owns the Installed apps entry.
    bool managed = false;
};

// Whether this Windows has the Windows 11 virtual camera API.
bool VirtualCamerasSupported();

int CmdInstall(const InstallOptions& options);
int CmdUninstall(bool managed = false);
int CmdStatus();
int CmdConfig(Args& args);
int CmdPlaceholder(Args& args);
// The picture apps see while nothing feeds a camera (needs administrator rights).
bool SetPlaceholderImage(const std::wstring& file, std::string* error);
bool ResetPlaceholderImage(std::string* error);
int CmdRestartFrameServer();
int CmdAutostart(Args& args, int argc, wchar_t** argv);

// Re-creates any registered camera Windows lost (needs elevation; quiet otherwise).
void EnsureCamerasRegistered();

// --- frame sources ---------------------------------------------------------------------

// What the camera currently streams; 0 when no application has asked yet.
struct Target {
    int width = 0, height = 0;
    double fps = 0;
    PixelFormat format = PixelFormat::Unknown;
};

struct Frame {
    PixelFormat format = PixelFormat::Unknown;
    int width = 0, height = 0;
    const uint8_t* planes[3] = {};
    int pitches[3] = {};
    ColorSpace colorspace = ColorSpace::Bt601Limited;
    int64_t timestamp = 0;  // QPC ticks, 0 = now
};

class Source {
public:
    virtual ~Source() = default;
    // Set while the owner wants the source to stop; long waits inside Next() check it.
    const std::atomic<bool>* stop_flag = nullptr;
    bool stopping() const;
    virtual const char* name() const = 0;
    // Opens devices/files. On failure, *error says why.
    virtual bool Start(std::string* error) = 0;
    // Produces the next frame, blocking for at most ~timeout_ms. Returns false
    // when there is no frame this time (timeout); *ended is set at end of input.
    // The frame stays valid until the next call.
    virtual bool Next(const Target& target, int timeout_ms, Frame* frame, bool* ended) = 0;
    // A source whose picture never changes: sent once, and again when the
    // camera's format changes.
    virtual bool IsStatic() const { return false; }
    // Sources that pace themselves (capture, video) return true; generated ones
    // are paced by the loop at the target frame rate.
    virtual bool SelfPaced() const { return false; }
    // Stop/restart capture while no application watches.
    virtual void Pause() {}
    virtual void Resume() {}
};

struct SourceOptions {
    int camera = 1;
    ScaleMode scale = ScaleMode::Fit;
    bool mirror = false;
    bool flip = false;
    Rgb background;
    double fps = 0;          // 0: follow the camera
    int width = 0, height = 0;  // render size for generated sources; 0: follow the camera
    bool cursor = true;
    bool loop = true;
    bool always = false;     // keep producing while nobody watches
    bool quiet = false;
};

std::unique_ptr<Source> MakeBarsSource(const SourceOptions& options);
std::unique_ptr<Source> MakeColorSource(Rgb color);
std::unique_ptr<Source> MakeImageSource(const std::wstring& path);
std::unique_ptr<Source> MakeVideoSource(const std::wstring& path, bool loop);
std::unique_ptr<Source> MakeWebcamSource(const std::string& selector);
std::unique_ptr<Source> MakeScreenSource(int monitor, bool cursor);
std::unique_ptr<Source> MakeWindowSource(const std::string& title, bool cursor);
std::unique_ptr<Source> MakeWindowSourceFor(HWND window, bool cursor);
// Screen/window capture scales on the GPU, so it needs the placement up front.
void ConfigureCapture(Source& source, ScaleMode scale, Rgb background);

// --- running a source ---------------------------------------------------------------

using SourceFactory = std::function<std::unique_ptr<Source>()>;

struct RunnerStats {
    bool running = false;   // started and not yet ended
    bool watching = false;  // an application is streaming the camera (or `always`)
    bool ended = false;     // the source finished: end of the file, window closed, error
    std::string error;      // why it ended, when it failed
    vcam_status camera{};   // what the camera streams, from the SDK
    double fps = 0;         // frames sent per second
    double ms_per_frame = 0;  // time spent converting each frame into the camera
    uint64_t frames = 0;
};

// Feeds one camera from one source on a thread of its own. The source is made
// and used on that thread (Media Foundation and capture need their COM
// apartment there).
class SourceRunner {
public:
    SourceRunner(SourceFactory factory, const SourceOptions& options);
    SourceRunner(const SourceRunner&) = delete;
    SourceRunner& operator=(const SourceRunner&) = delete;
    ~SourceRunner();

    // Opens the source and the camera; returns once both work, or false with *error.
    bool Start(std::string* error);
    void Stop();
    RunnerStats Stats() const;
    std::string name() const;
    const SourceOptions& options() const { return options_; }

    // Called on the runner's thread with every frame sent (for previews), and
    // the camera's current size (0 when no application asked yet).
    using FrameTap = std::function<void(const Frame& frame, const Target& target)>;
    void SetTap(FrameTap tap);
    // Keep producing although no application watches (the GUI's preview is visible).
    void SetKeepAlive(bool keep) { keep_alive_ = keep; }

private:
    void Run(std::promise<std::string>* started);

    SourceFactory factory_;
    SourceOptions options_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> keep_alive_{false};
    mutable std::mutex lock_;  // everything below
    RunnerStats stats_;
    std::string name_;
    FrameTap tap_;
};

// The command-line loop: runs the source until Ctrl+C, with a status line.
int RunSource(SourceFactory factory, const SourceOptions& options);
int CmdList();

struct MonitorInfo {
    int number = 0;  // 1-based, as `vcam screen N` takes it
    std::wstring device;
    RECT rect{};
    bool primary = false;
};
std::vector<MonitorInfo> EnumerateMonitors();

struct WindowInfo {
    HWND handle = nullptr;
    std::wstring title;
    std::wstring process;  // executable name
    int width = 0, height = 0;
};
std::vector<WindowInfo> EnumerateWindows();

// Loads any image WIC can decode as top-down BGRA, applying EXIF orientation.
bool LoadImageBgra(const std::wstring& path, std::vector<uint8_t>* pixels, int* width, int* height,
                   std::string* error);

// Physical cameras (excluding vcam's own), for `list` and the webcam source.
struct CameraInfo {
    std::wstring name;
    std::wstring symbolic_link;
    bool is_vcam = false;
};
std::vector<CameraInfo> EnumerateCameras();

}  // namespace vcam::app
