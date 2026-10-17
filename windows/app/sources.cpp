// The producer loop and the simple sources: test pattern, solid colour, image.
#include "app.h"

#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "vcam/draw.h"

using Microsoft::WRL::ComPtr;

namespace vcam::app {

namespace {

Frame BgraFrame(const std::vector<uint8_t>& pixels, int width, int height)
{
    Frame f;
    f.format = PixelFormat::BGRA;
    f.width = width;
    f.height = height;
    f.planes[0] = pixels.data();
    f.pitches[0] = width * 4;
    return f;
}

// --- test pattern ------------------------------------------------------------------------

class BarsSource final : public Source {
public:
    explicit BarsSource(const SourceOptions& options) : width_(options.width), height_(options.height) {}
    const char* name() const override { return "colour bars"; }
    bool Start(std::string*) override
    {
        start_ = win::NowTicks();
        return true;
    }

    bool Next(const Target& target, int, Frame* frame, bool*) override
    {
        const int w = width_ ? width_ : (target.width ? target.width : 1280);
        const int h = height_ ? height_ : (target.height ? target.height : 720);
        pixels_.resize(static_cast<size_t>(w) * h * 4);
        const double seconds = static_cast<double>(win::NowTicks() - start_) / shm::kTicksPerSecond;

        SYSTEMTIME t;
        GetLocalTime(&t);
        char line2[96], line3[96];
        snprintf(line2, sizeof(line2), "%dx%d %s %.4g fps", w, h,
                 target.format != PixelFormat::Unknown ? FormatName(target.format) : "", target.fps);
        snprintf(line3, sizeof(line3), "%02u:%02u:%02u.%03u  frame %llu", t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds, static_cast<unsigned long long>(frame_));
        const char* lines[] = {"VCAM TEST PATTERN", line2, line3};
        RenderTestPattern(MakeSpan(PixelFormat::BGRA, w, h, pixels_.data()), frame_, seconds, lines, 3);
        frame_++;
        *frame = BgraFrame(pixels_, w, h);
        return true;
    }

private:
    int width_, height_;
    int64_t start_ = 0;
    uint64_t frame_ = 0;
    std::vector<uint8_t> pixels_;
};

// --- solid colour ------------------------------------------------------------------------------

class ColorSource final : public Source {
public:
    explicit ColorSource(Rgb color) : color_(color) {}
    const char* name() const override { return "solid colour"; }
    bool Start(std::string*) override { return true; }
    bool IsStatic() const override { return true; }

    bool Next(const Target& target, int, Frame* frame, bool*) override
    {
        // At the camera's own size, so --fit has nothing to pad.
        const int w = target.width ? target.width : 1280;
        const int h = target.height ? target.height : 720;
        pixels_.resize(static_cast<size_t>(w) * h * 4);
        FillImage(MakeSpan(PixelFormat::BGRA, w, h, pixels_.data()), color_, ColorSpace::Bt601Limited);
        *frame = BgraFrame(pixels_, w, h);
        return true;
    }

private:
    Rgb color_;
    std::vector<uint8_t> pixels_;
};

// --- image -----------------------------------------------------------------------------------------

class ImageSource final : public Source {
public:
    explicit ImageSource(std::wstring path) : path_(std::move(path)) {}
    const char* name() const override { return "image"; }
    bool IsStatic() const override { return true; }
    bool Start(std::string* error) override { return LoadImageBgra(path_, &pixels_, &width_, &height_, error); }
    bool Next(const Target&, int, Frame* frame, bool*) override
    {
        *frame = BgraFrame(pixels_, width_, height_);
        return true;
    }

private:
    std::wstring path_;
    std::vector<uint8_t> pixels_;
    int width_ = 0, height_ = 0;
};

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

void ShowStatus(const SourceOptions& o, const vcam_status& st, double sent_fps, double ms_per_frame, bool waiting)
{
    char line[200];
    if (waiting) {
        snprintf(line, sizeof(line), "camera %d: waiting for an app to open the camera...", o.camera);
    } else {
        const double fps = st.fps_denominator ? static_cast<double>(st.fps_numerator) / st.fps_denominator : 0;
        snprintf(line, sizeof(line), "camera %d: apps get %dx%d %s @ %.4g fps | sending %.1f fps, %.1f ms/frame | %llu delivered",
                 o.camera, st.width, st.height,
                 st.format ? FormatName(static_cast<PixelFormat>(st.format)) : "?", fps, sent_fps, ms_per_frame,
                 static_cast<unsigned long long>(st.frames_delivered));
    }
    Print("\r%-118s", line);
}

}  // namespace

std::unique_ptr<Source> MakeBarsSource(const SourceOptions& options)
{
    return std::make_unique<BarsSource>(options);
}

std::unique_ptr<Source> MakeColorSource(Rgb color)
{
    return std::make_unique<ColorSource>(color);
}

std::unique_ptr<Source> MakeImageSource(const std::wstring& path)
{
    return std::make_unique<ImageSource>(path);
}

bool LoadImageBgra(const std::wstring& path, std::vector<uint8_t>* pixels, int* width, int* height, std::string* error)
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        *error = "cannot read " + Utf8(path) + ": " + HresultText(hr);
        if (com) CoUninitialize();
        return false;
    }

    // Phone photos store their rotation in EXIF instead of in the pixels.
    ComPtr<IWICBitmapSource> source = frame;
    ComPtr<IWICMetadataQueryReader> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) && value.vt == VT_UI2) {
            WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
            switch (value.uiVal) {
            case 2: transform = WICBitmapTransformFlipHorizontal; break;
            case 3: transform = WICBitmapTransformRotate180; break;
            case 4: transform = WICBitmapTransformFlipVertical; break;
            case 5: transform = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
            case 6: transform = WICBitmapTransformRotate90; break;
            case 7: transform = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
            case 8: transform = WICBitmapTransformRotate270; break;
            default: break;
            }
            ComPtr<IWICBitmapFlipRotator> rotator;
            if (transform != WICBitmapTransformRotate0 && SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) &&
                SUCCEEDED(rotator->Initialize(frame.Get(), transform))) {
                source = rotator;
            }
        }
        PropVariantClear(&value);
    }

    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension)) {
        hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr)) {
        pixels->resize(static_cast<size_t>(w) * h * 4);
        hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels->size()), pixels->data());
    }
    if (com) CoUninitialize();
    if (FAILED(hr)) {
        *error = "cannot decode " + Utf8(path) + ": " + HresultText(hr);
        return false;
    }
    // Composite transparent pixels onto black, as apps would show them.
    for (size_t i = 0; i < pixels->size(); i += 4) {
        const unsigned a = (*pixels)[i + 3];
        if (a != 255) {
            for (int c = 0; c < 3; c++) {
                (*pixels)[i + c] = static_cast<uint8_t>((*pixels)[i + c] * a / 255);
            }
        }
    }
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
}

int RunSource(Source& source, const SourceOptions& o)
{
    EnsureCamerasRegistered();
    std::string error;
    if (!source.Start(&error)) {
        PrintError("%s\n", error.c_str());
        return 1;
    }

    vcam_options vo = VCAM_OPTIONS_INIT;
    vo.camera = o.camera;
    vo.scale = static_cast<int32_t>(o.scale);
    vo.mirror = o.mirror;
    vo.flip = o.flip;
    vo.background = (static_cast<uint32_t>(o.background.r) << 16) | (static_cast<uint32_t>(o.background.g) << 8) |
                    o.background.b;
    vcam_producer* producer = nullptr;
    const int rc = vcam_open(&vo, &producer);
    if (rc != VCAM_OK) {
        PrintError("camera %d: %s\n", o.camera, vcam_error_string(rc));
        return 1;
    }
    const win::Config config = win::LoadConfig();
    if (!o.quiet) {
        Print("Feeding camera %d (\"%s\") with %s. Ctrl+C stops.\n", o.camera,
              Utf8(config.names[o.camera - 1]).c_str(), source.name());
        if (config.cameras > 0 && o.camera > config.cameras) {
            Print("Note: only %d camera%s installed; apps will not list camera %d (`vcam install --cameras %d`).\n",
                  config.cameras, config.cameras == 1 ? " is" : "s are", o.camera, o.camera);
        }
    }

    Ticker ticker;
    int64_t next_tick = win::NowTicks();
    int64_t last_status = 0, window_start = win::NowTicks();
    int window_frames = 0;
    double window_ms = 0, shown_fps = 0, shown_ms = 0;
    bool paused = false;
    Target sent_static{};
    bool sent_any_static = false;
    int exit_code = 0;

    while (!g_stop) {
        vcam_status st{};
        st.struct_size = sizeof(st);
        vcam_get_status(producer, &st);
        Target target;
        target.width = st.width;
        target.height = st.height;
        target.format = static_cast<PixelFormat>(st.format);
        target.fps = o.fps ? o.fps
                           : (st.fps_denominator ? static_cast<double>(st.fps_numerator) / st.fps_denominator : 30.0);
        if (target.fps <= 0) {
            target.fps = 30;
        }
        const bool watching = st.consumer_active || o.always;
        const int64_t now = win::NowTicks();

        if (!o.quiet && now - last_status > shm::kTicksPerSecond / 2) {
            const double seconds = static_cast<double>(now - window_start) / shm::kTicksPerSecond;
            if (seconds > 0.9) {
                shown_fps = window_frames / seconds;
                shown_ms = window_frames ? window_ms / window_frames : 0;
                window_frames = 0;
                window_ms = 0;
                window_start = now;
            }
            ShowStatus(o, st, shown_fps, shown_ms, !watching && !source.IsStatic());
            last_status = now;
        }

        Frame frame;
        bool ended = false;
        if (source.IsStatic()) {
            // Send once, and again when the camera's format changes.
            const bool changed = !sent_any_static || target.width != sent_static.width ||
                                 target.height != sent_static.height || target.format != sent_static.format;
            if (!changed || !source.Next(target, 0, &frame, &ended)) {
                Sleep(100);
                continue;
            }
            sent_static = target;
            sent_any_static = true;
        } else {
            if (!watching) {
                if (!paused) {
                    source.Pause();
                    paused = true;
                }
                Sleep(100);
                continue;
            }
            if (paused) {
                source.Resume();
                paused = false;
                next_tick = win::NowTicks();
            }
            if (!source.SelfPaced()) {
                const int64_t period = static_cast<int64_t>(shm::kTicksPerSecond / target.fps);
                next_tick += period;
                if (next_tick < win::NowTicks() - period) {
                    next_tick = win::NowTicks();  // fell behind: do not try to catch up
                }
                ticker.WaitUntil(next_tick);
            }
            if (!source.Next(target, 100, &frame, &ended)) {
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
            PrintError("\nsending a frame failed: %s\n", vcam_error_string(sent));
            exit_code = 1;
            break;
        }
        window_ms += static_cast<double>(win::NowTicks() - start) / 10000.0;
        window_frames++;
    }
    vcam_close(producer);
    if (!o.quiet) {
        Print("\nStopped.\n");
    }
    return exit_code;
}

int CmdList()
{
    Print("Monitors (vcam screen N):\n");
    struct MonitorList {
        int n = 0;
    } list;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* l = reinterpret_cast<MonitorList*>(data);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            const RECT& r = info.rcMonitor;
            Print("  %d  %ldx%ld at (%ld, %ld)%s  %s\n", ++l->n, r.right - r.left, r.bottom - r.top, r.left, r.top,
                  (info.dwFlags & MONITORINFOF_PRIMARY) ? "  primary" : "", Utf8(info.szDevice).c_str());
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));

    Print("\nWindows (vcam window TITLE):\n");
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOTOWNER) != hwnd ||
                (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
                return TRUE;
            }
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            wchar_t title[256] = {};
            if (cloaked || GetWindowTextW(hwnd, title, 256) == 0) {
                return TRUE;
            }
            RECT r{};
            GetClientRect(hwnd, &r);
            Print("  %-60s %ldx%ld\n", Utf8(title).c_str(), r.right, r.bottom);
            return TRUE;
        },
        0);

    Print("\nCameras (vcam webcam N|NAME):\n");
    int n = 0;
    for (const CameraInfo& c : EnumerateCameras()) {
        if (c.is_vcam) {
            Print("  -  %s  (vcam)\n", Utf8(c.name).c_str());
        } else {
            Print("  %d  %s\n", ++n, Utf8(c.name).c_str());
        }
    }
    if (n == 0) {
        Print("  (no physical cameras)\n");
    }
    return 0;
}

}  // namespace vcam::app
