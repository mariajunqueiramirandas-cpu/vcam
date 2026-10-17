// The simple sources (test pattern, solid colour, image) and listing what can be captured.
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

std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(data);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            MonitorInfo m;
            m.number = static_cast<int>(list->size()) + 1;
            m.device = info.szDevice;
            m.rect = info.rcMonitor;
            m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            list->push_back(m);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

std::vector<WindowInfo> EnumerateWindows()
{
    struct Search {
        std::vector<WindowInfo> windows;
        HWND console = GetConsoleWindow();
    } search;
    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            auto* s = reinterpret_cast<Search*>(data);
            if (hwnd == s->console || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOTOWNER) != hwnd ||
                (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
                return TRUE;
            }
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            wchar_t title[256] = {};
            if (cloaked || GetWindowTextW(hwnd, title, 256) == 0) {
                return TRUE;
            }
            WindowInfo w;
            w.handle = hwnd;
            w.title = title;
            RECT r{};
            GetClientRect(hwnd, &r);
            w.width = r.right;
            w.height = r.bottom;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH] = {};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                    const std::wstring full(path);
                    w.process = full.substr(full.find_last_of(L'\\') + 1);
                }
                CloseHandle(process);
            }
            s->windows.push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.windows;
}

int CmdList()
{
    Print("Monitors (vcam screen N):\n");
    for (const MonitorInfo& m : EnumerateMonitors()) {
        const RECT& r = m.rect;
        Print("  %d  %ldx%ld at (%ld, %ld)%s  %s\n", m.number, r.right - r.left, r.bottom - r.top, r.left, r.top,
              m.primary ? "  primary" : "", Utf8(m.device).c_str());
    }

    Print("\nWindows (vcam window TITLE):\n");
    for (const WindowInfo& w : EnumerateWindows()) {
        Print("  %-60s %dx%d\n", Utf8(w.title).c_str(), w.width, w.height);
    }

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
