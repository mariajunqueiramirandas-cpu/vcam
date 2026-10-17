// Running a source: the producer loop shared by the command line and the GUI.
#include "app.h"

#include <objbase.h>

#include <cstdio>

namespace vcam::app {

namespace {

// How long nobody may watch before a source pauses: a camera switching
// formats, or one application handing over to another, should not close a
// webcam just to reopen it.
constexpr int64_t kPauseDelay = shm::kTicksPerSecond;

// Waits until an absolute QPC tick with a high-resolution timer.
class Ticker {
public:
    Ticker()
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }
    Ticker(const Ticker&) = delete;
    Ticker& operator=(const Ticker&) = delete;
    ~Ticker()
    {
        if (timer_) {
            CloseHandle(timer_);
        }
    }
    void WaitUntil(int64_t tick)
    {
        const int64_t delay = tick - win::NowTicks();
        if (delay <= 0) {
            return;
        }
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -delay;
            SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer_, static_cast<DWORD>(delay / 10000 + 100));
        } else {
            Sleep(static_cast<DWORD>(delay / 10000));
        }
    }

private:
    HANDLE timer_ = nullptr;
};

void ShowStatus(const SourceOptions& o, const RunnerStats& s)
{
    char line[200];
    const vcam_status& st = s.camera;
    if (!s.watching) {
        snprintf(line, sizeof(line), "camera %d: waiting for an app to open the camera...", o.camera);
    } else {
        const double fps = st.fps_denominator ? static_cast<double>(st.fps_numerator) / st.fps_denominator : 0;
        snprintf(line, sizeof(line), "camera %d: apps get %dx%d %s @ %.4g fps | sending %.1f fps, %.1f ms/frame | %llu delivered",
                 o.camera, st.width, st.height,
                 st.format ? FormatName(static_cast<PixelFormat>(st.format)) : "?", fps, s.fps, s.ms_per_frame,
                 static_cast<unsigned long long>(st.frames_delivered));
    }
    Print("\r%-118s", line);
}

}  // namespace

SourceRunner::SourceRunner(SourceFactory factory, const SourceOptions& options)
    : factory_(std::move(factory)), options_(options)
{
}

SourceRunner::~SourceRunner()
{
    Stop();
}

bool SourceRunner::Start(std::string* error)
{
    if (thread_.joinable()) {
        return true;
    }
    stop_ = false;
    std::promise<std::string> started;
    std::future<std::string> result = started.get_future();
    thread_ = std::thread(&SourceRunner::Run, this, &started);
    const std::string message = result.get();
    if (!message.empty()) {
        thread_.join();
        if (error) {
            *error = message;
        }
        return false;
    }
    return true;
}

void SourceRunner::Stop()
{
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

RunnerStats SourceRunner::Stats() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return stats_;
}

std::string SourceRunner::name() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return name_;
}

void SourceRunner::SetTap(FrameTap tap)
{
    std::lock_guard<std::mutex> lock(lock_);
    tap_ = std::move(tap);
}

void SourceRunner::Run(std::promise<std::string>* started)
{
    SetThreadDescription(GetCurrentThread(), L"vcam source");
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::unique_ptr<Source> source = factory_ ? factory_() : nullptr;
    std::string error;
    vcam_producer* producer = nullptr;
    if (!source) {
        error = "no source";
    } else {
        source->stop_flag = &stop_;
        ConfigureCapture(*source, options_.scale, options_.background);
        if (source->Start(&error)) {
            vcam_options vo = VCAM_OPTIONS_INIT;
            vo.camera = options_.camera;
            vo.scale = static_cast<int32_t>(options_.scale);
            vo.mirror = options_.mirror;
            vo.flip = options_.flip;
            vo.background = (static_cast<uint32_t>(options_.background.r) << 16) |
                            (static_cast<uint32_t>(options_.background.g) << 8) | options_.background.b;
            const int rc = vcam_open(&vo, &producer);
            if (rc != VCAM_OK) {
                error = "camera " + std::to_string(options_.camera) + ": " + vcam_error_string(rc);
            }
        } else if (error.empty()) {
            error = std::string("cannot start ") + source->name();
        }
    }
    if (!error.empty()) {
        source.reset();
        if (com) CoUninitialize();
        started->set_value(error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(lock_);
        name_ = source->name();
        stats_ = RunnerStats{};
        stats_.running = true;
    }
    started->set_value(std::string());

    Ticker ticker;
    int64_t next_tick = win::NowTicks();
    int64_t window_start = win::NowTicks(), unwatched_since = 0;
    int window_frames = 0;
    double window_ms = 0;
    bool paused = false;
    Target sent_static{};
    bool sent_any_static = false;
    std::string failure;

    while (!stop_) {
        vcam_status st{};
        st.struct_size = sizeof(st);
        vcam_get_status(producer, &st);
        Target target;
        target.width = st.width;
        target.height = st.height;
        target.format = static_cast<PixelFormat>(st.format);
        target.fps = options_.fps ? options_.fps
                                  : (st.fps_denominator ? static_cast<double>(st.fps_numerator) / st.fps_denominator
                                                        : 30.0);
        if (target.fps <= 0) {
            target.fps = 30;
        }
        const int64_t now = win::NowTicks();
        if (st.consumer_active || options_.always || keep_alive_) {
            unwatched_since = 0;
        } else if (!unwatched_since) {
            unwatched_since = now;
        }
        const bool watching = !unwatched_since || now - unwatched_since < kPauseDelay;
        {
            std::lock_guard<std::mutex> lock(lock_);
            stats_.camera = st;
            stats_.watching = watching;
            const double seconds = static_cast<double>(now - window_start) / shm::kTicksPerSecond;
            if (seconds >= 1.0) {
                stats_.fps = window_frames / seconds;
                stats_.ms_per_frame = window_frames ? window_ms / window_frames : 0;
                window_frames = 0;
                window_ms = 0;
                window_start = now;
            }
        }

        Frame frame;
        bool ended = false;
        if (source->IsStatic()) {
            // Send once, and again when the camera's format changes.
            const bool changed = !sent_any_static || target.width != sent_static.width ||
                                 target.height != sent_static.height || target.format != sent_static.format;
            if (!changed || !source->Next(target, 0, &frame, &ended)) {
                Sleep(100);
                continue;
            }
            sent_static = target;
            sent_any_static = true;
        } else {
            if (!watching) {
                if (!paused) {
                    source->Pause();
                    paused = true;
                }
                Sleep(100);
                continue;
            }
            if (paused) {
                source->Resume();
                paused = false;
                next_tick = win::NowTicks();
            }
            if (!source->SelfPaced()) {
                const int64_t period = static_cast<int64_t>(shm::kTicksPerSecond / target.fps);
                next_tick += period;
                if (next_tick < win::NowTicks() - period) {
                    next_tick = win::NowTicks();  // fell behind: do not try to catch up
                }
                ticker.WaitUntil(next_tick);
            }
            if (!source->Next(target, 100, &frame, &ended)) {
                if (ended) {
                    break;
                }
                continue;
            }
        }

        const int64_t start = win::NowTicks();
        vcam_frame vf{};
        vf.format = static_cast<int32_t>(frame.format);
        vf.width = frame.width;
        vf.height = frame.height;
        for (int i = 0; i < 3; i++) {
            vf.planes[i] = frame.planes[i];
            vf.pitches[i] = frame.pitches[i];
        }
        vf.colorspace = static_cast<int32_t>(frame.colorspace);
        vf.timestamp = frame.timestamp;
        const int sent = vcam_send(producer, &vf);
        if (sent != VCAM_OK) {
            failure = std::string("sending a frame failed: ") + vcam_error_string(sent);
            break;
        }
        window_ms += static_cast<double>(win::NowTicks() - start) / 10000.0;
        window_frames++;
        FrameTap tap;
        {
            std::lock_guard<std::mutex> lock(lock_);
            stats_.frames++;
            tap = tap_;
        }
        if (tap) {
            tap(frame, target);
        }
    }

    vcam_close(producer);
    source.reset();
    if (com) CoUninitialize();
    std::lock_guard<std::mutex> lock(lock_);
    stats_.running = false;
    stats_.ended = !stop_;
    stats_.error = failure;
}

int RunSource(SourceFactory factory, const SourceOptions& o)
{
    EnsureCamerasRegistered();
    SourceRunner runner(std::move(factory), o);
    std::string error;
    if (!runner.Start(&error)) {
        PrintError("%s\n", error.c_str());
        return 1;
    }
    const win::Config config = win::LoadConfig();
    if (!o.quiet) {
        Print("Feeding camera %d (\"%s\") with %s. Ctrl+C stops.\n", o.camera,
              Utf8(config.names[o.camera - 1]).c_str(), runner.name().c_str());
        if (config.cameras > 0 && o.camera > config.cameras) {
            Print("Note: only %d camera%s installed; apps will not list camera %d (`vcam install --cameras %d`).\n",
                  config.cameras, config.cameras == 1 ? " is" : "s are", o.camera, o.camera);
        }
    }
    RunnerStats stats;
    while (!g_stop) {
        stats = runner.Stats();
        if (!stats.running) {
            break;
        }
        if (!o.quiet) {
            ShowStatus(o, stats);
        }
        for (int i = 0; i < 5 && !g_stop; i++) {
            Sleep(100);
        }
    }
    runner.Stop();
    stats = runner.Stats();
    if (!stats.error.empty()) {
        PrintError("\n%s\n", stats.error.c_str());
    }
    if (!o.quiet) {
        Print("\nStopped.\n");
    }
    return stats.error.empty() ? 0 : 1;
}

}  // namespace vcam::app
