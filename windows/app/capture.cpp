// Screen and window capture with Windows.Graphics.Capture. Frames are scaled and
// converted to NV12 on the GPU (D3D11 video processor) at exactly the size the
// camera streams, so the CPU only copies the finished picture.
#include "app.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace vcam::app {

namespace {

// BGRA texture -> NV12 texture of the camera's size, with fit/fill/stretch placement.
class GpuScaler {
public:
    bool Configure(ID3D11Device* device, ID3D11DeviceContext* context, UINT in_w, UINT in_h, UINT out_w, UINT out_h)
    {
        if (ready_ && in_w == in_w_ && in_h == in_h_ && out_w == out_w_ && out_h == out_h_) {
            return true;
        }
        Reset();
        ComPtr<ID3D11VideoDevice> vdev;
        ComPtr<ID3D11VideoContext> vctx;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&vdev))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&vctx)))) {
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {60, 1};
        desc.InputWidth = in_w;
        desc.InputHeight = in_h;
        desc.OutputFrameRate = {60, 1};
        desc.OutputWidth = out_w;
        desc.OutputHeight = out_h;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        UINT support = 0;
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&desc, &enumerator)) ||
            FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ||
            FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
            return false;
        }
        ComPtr<ID3D11VideoProcessor> processor;
        if (FAILED(vdev->CreateVideoProcessor(enumerator.Get(), 0, &processor))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = out_w;
        td.Height = out_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> output, staging;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &output))) {
            return false;
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) {
            return false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output_view;
        if (FAILED(vdev->CreateVideoProcessorOutputView(output.Get(), enumerator.Get(), &ovd, &output_view))) {
            return false;
        }

        // sRGB-ish full-range RGB in, BT.601 limited-range YUV out (what the camera declares).
        ComPtr<ID3D11VideoContext1> vctx1;
        if (SUCCEEDED(vctx.As(&vctx1))) {
            vctx1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            vctx1->VideoProcessorSetOutputColorSpace1(processor.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
            in.RGB_Range = 0;
            out.YCbCr_Matrix = 0;
            out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            vctx->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &in);
            vctx->VideoProcessorSetOutputColorSpace(processor.Get(), &out);
        }
        vctx->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        vctx->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        vdev_ = vdev;
        vctx_ = vctx;
        enumerator_ = enumerator;
        processor_ = processor;
        output_ = output;
        staging_ = staging;
        output_view_ = output_view;
        in_w_ = in_w;
        in_h_ = in_h;
        out_w_ = out_w;
        out_h_ = out_h;
        ready_ = true;
        return true;
    }

    // Scales `texture` (content rectangle src) into the output and maps it.
    bool Process(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const RECT& src, const RECT& dst,
                 Rgb background, Frame* frame)
    {
        Unmap(context);
        ComPtr<ID3D11VideoProcessorInputView> input;
        auto it = input_views_.find(texture);
        if (it != input_views_.end()) {
            input = it->second;
        } else {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            if (FAILED(vdev_->CreateVideoProcessorInputView(texture, enumerator_.Get(), &ivd, &input))) {
                return false;
            }
            if (input_views_.size() > 8) {
                input_views_.clear();
            }
            input_views_[texture] = input;
        }
        const RECT full{0, 0, static_cast<LONG>(out_w_), static_cast<LONG>(out_h_)};
        D3D11_VIDEO_COLOR color{};
        color.RGBA = {background.r / 255.0f, background.g / 255.0f, background.b / 255.0f, 1.0f};
        vctx_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &color);
        vctx_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &full);
        vctx_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &src);
        vctx_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dst);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.Get();
        if (FAILED(vctx_->VideoProcessorBlt(processor_.Get(), output_view_.Get(), 0, 1, &stream))) {
            return false;
        }
        context->CopyResource(staging_.Get(), output_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        mapped_ = true;
        const auto* y = static_cast<const uint8_t*>(mapped.pData);
        frame->format = PixelFormat::NV12;
        frame->width = static_cast<int>(out_w_);
        frame->height = static_cast<int>(out_h_);
        frame->planes[0] = y;
        frame->planes[1] = y + static_cast<size_t>(mapped.RowPitch) * out_h_;
        frame->pitches[0] = frame->pitches[1] = static_cast<int>(mapped.RowPitch);
        frame->colorspace = ColorSpace::Bt601Limited;
        return true;
    }

    void Unmap(ID3D11DeviceContext* context)
    {
        if (mapped_) {
            context->Unmap(staging_.Get(), 0);
            mapped_ = false;
        }
    }

    void Reset()
    {
        ready_ = false;
        input_views_.clear();
        output_view_.Reset();
        staging_.Reset();
        output_.Reset();
        processor_.Reset();
        enumerator_.Reset();
        vctx_.Reset();
        vdev_.Reset();
    }

    bool mapped() const { return mapped_; }

private:
    bool ready_ = false, mapped_ = false;
    UINT in_w_ = 0, in_h_ = 0, out_w_ = 0, out_h_ = 0;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> output_, staging_;
    ComPtr<ID3D11VideoProcessorOutputView> output_view_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> input_views_;
};

class CaptureSource final : public Source {
public:
    CaptureSource(HMONITOR monitor, HWND window, bool cursor, std::string what)
        : monitor_(monitor), window_(window), cursor_(cursor), what_(std::move(what))
    {
    }
    ~CaptureSource() override { StopCapture(); }
    const char* name() const override { return what_.c_str(); }
    bool SelfPaced() const override { return true; }

    bool Start(std::string* error) override
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error&) {
        }
        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) {
                *error = "screen capture is not supported on this system";
                return false;
            }
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                           D3D11_SDK_VERSION, &device_, nullptr, &context_);
            if (FAILED(hr)) {
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                       0, D3D11_SDK_VERSION, &device_, nullptr, &context_);
            }
            winrt::check_hresult(hr);
            ComPtr<IDXGIDevice> dxgi;
            winrt::check_hresult(device_.As(&dxgi));
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
            d3d_ = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            if (window_) {
                winrt::check_hresult(interop->CreateForWindow(window_, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                              winrt::put_abi(item_)));
            } else {
                winrt::check_hresult(interop->CreateForMonitor(monitor_, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                               winrt::put_abi(item_)));
            }
            closed_ = item_.Closed(winrt::auto_revoke, [this](auto&&, auto&&) { item_closed_ = true; });
            frame_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            StartCapture();
            return true;
        } catch (const winrt::hresult_error& e) {
            *error = "cannot capture " + what_ + ": " + Utf8(std::wstring(e.message())) + " (" +
                     HresultText(e.code()) + ")";
            return false;
        }
    }

    bool Next(const Target& target, int timeout_ms, Frame* frame, bool* ended) override
    {
        if (item_closed_) {
            PrintError("\nthe captured window was closed\n");
            *ended = true;
            return false;
        }
        if (!session_) {
            StartCapture();
        }
        if (WaitForSingleObject(frame_event_, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
            return false;  // nothing new (e.g. a minimised window): the camera repeats the last frame
        }
        try {
            // Only the newest frame matters.
            wgc::Direct3D11CaptureFrame latest{nullptr};
            while (auto f = pool_.TryGetNextFrame()) {
                latest = f;
            }
            if (!latest) {
                return false;
            }
            const auto content = latest.ContentSize();
            if (content.Width != size_.Width || content.Height != size_.Height) {
                // The window was resized: recreate the pool at the new size.
                size_ = content;
                pool_.Recreate(d3d_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
            }
            auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            const int cw = std::min<int>(content.Width, static_cast<int>(desc.Width));
            const int ch = std::min<int>(content.Height, static_cast<int>(desc.Height));
            if (cw < 2 || ch < 2) {
                return false;
            }
            if (target.width >= 16 && target.height >= 16 && !gpu_failed_ && GpuFrame(texture.Get(), cw, ch, target, frame)) {
                return true;
            }
            return CpuFrame(texture.Get(), cw, ch, frame);
        } catch (const winrt::hresult_error& e) {
            PrintError("\ncapture failed: %s\n", HresultText(e.code()).c_str());
            *ended = true;
            return false;
        }
    }

    void Pause() override { StopCapture(); }
    void Resume() override {}

    // Set by RunScreenSource before starting: how to place the picture on the GPU.
    ScaleMode scale = ScaleMode::Fit;
    Rgb background;

private:
    void StartCapture()
    {
        size_ = item_.Size();
        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                    2, size_);
        frame_arrived_ = pool_.FrameArrived(winrt::auto_revoke, [this](auto&&, auto&&) { SetEvent(frame_event_); });
        session_ = pool_.CreateCaptureSession(item_);
        try {
            session_.IsCursorCaptureEnabled(cursor_);
        } catch (const winrt::hresult_error&) {
        }
        try {
            // Windows 11: no yellow border around what is being captured. The
            // request is granted without a prompt to unpackaged apps.
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
                if (!borderless_requested_) {
                    borderless_requested_ = true;
                    wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
                }
                session_.IsBorderRequired(false);
            }
        } catch (const winrt::hresult_error&) {
        }
        session_.StartCapture();
    }

    void StopCapture()
    {
        if (context_ && staging_mapped_) {
            context_->Unmap(staging_.Get(), 0);
            staging_mapped_ = false;
        }
        if (context_) {
            scaler_.Unmap(context_.Get());
        }
        frame_arrived_.revoke();
        if (session_) {
            session_.Close();
            session_ = nullptr;
        }
        if (pool_) {
            pool_.Close();
            pool_ = nullptr;
        }
    }

    bool GpuFrame(ID3D11Texture2D* texture, int cw, int ch, const Target& target, Frame* frame)
    {
        if (staging_mapped_) {
            context_->Unmap(staging_.Get(), 0);
            staging_mapped_ = false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!scaler_.Configure(device_.Get(), context_.Get(), desc.Width, desc.Height, static_cast<UINT>(target.width),
                               static_cast<UINT>(target.height))) {
            gpu_failed_ = true;  // no video processor: use the CPU path from now on
            return false;
        }
        const Placement pl = ComputePlacement(cw, ch, target.width, target.height, scale, true);
        RECT src{static_cast<LONG>(pl.src_x), static_cast<LONG>(pl.src_y), static_cast<LONG>(pl.src_x + pl.src_w),
                 static_cast<LONG>(pl.src_y + pl.src_h)};
        RECT dst{pl.dst_x, pl.dst_y, pl.dst_x + pl.dst_w, pl.dst_y + pl.dst_h};
        if (!scaler_.Process(context_.Get(), texture, src, dst, background, frame)) {
            gpu_failed_ = true;
            return false;
        }
        return true;
    }

    bool CpuFrame(ID3D11Texture2D* texture, int cw, int ch, Frame* frame)
    {
        scaler_.Unmap(context_.Get());
        if (staging_mapped_) {
            context_->Unmap(staging_.Get(), 0);
            staging_mapped_ = false;
        }
        if (!staging_ || staging_w_ != cw || staging_h_ != ch) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(cw);
            td.Height = static_cast<UINT>(ch);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_.Reset();
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
                return false;
            }
            staging_w_ = cw;
            staging_h_ = ch;
        }
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(cw), static_cast<UINT>(ch), 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        staging_mapped_ = true;
        frame->format = PixelFormat::BGRA;
        frame->width = cw;
        frame->height = ch;
        frame->planes[0] = static_cast<const uint8_t*>(mapped.pData);
        frame->pitches[0] = static_cast<int>(mapped.RowPitch);
        frame->colorspace = ColorSpace::Bt601Limited;
        return true;
    }

    HMONITOR monitor_;
    HWND window_;
    bool cursor_;
    std::string what_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    wgd::Direct3D11::IDirect3DDevice d3d_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 size_{};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived_;
    wgc::GraphicsCaptureItem::Closed_revoker closed_;
    HANDLE frame_event_ = nullptr;
    std::atomic<bool> item_closed_{false};
    GpuScaler scaler_;
    bool gpu_failed_ = false;
    bool borderless_requested_ = false;
    ComPtr<ID3D11Texture2D> staging_;
    int staging_w_ = 0, staging_h_ = 0;
    bool staging_mapped_ = false;
};

struct MonitorSearch {
    int wanted;
    int n = 0;
    HMONITOR found = nullptr;
};

struct WindowSearch {
    std::wstring needle;
    HWND found = nullptr;
    HWND own_console = nullptr;
};

std::wstring LowerText(std::wstring s)
{
    for (auto& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

// Stands in for a monitor or window that does not exist; Start() explains.
class MissingSource final : public Source {
public:
    explicit MissingSource(std::string message) : message_(std::move(message)) {}
    const char* name() const override { return "nothing"; }
    bool Start(std::string* error) override
    {
        *error = message_;
        return false;
    }
    bool Next(const Target&, int, Frame*, bool*) override { return false; }

private:
    std::string message_;
};

}  // namespace

std::unique_ptr<Source> MakeScreenSource(int monitor, bool cursor)
{
    HMONITOR handle = nullptr;
    if (monitor <= 0) {
        handle = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    } else {
        MonitorSearch search{monitor};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
                auto* s = reinterpret_cast<MonitorSearch*>(data);
                if (++s->n == s->wanted) {
                    s->found = m;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        handle = search.found;
    }
    if (!handle) {
        return std::make_unique<MissingSource>("there is no monitor " + std::to_string(monitor) + " (see `vcam list`)");
    }
    return std::make_unique<CaptureSource>(handle, nullptr, cursor,
                                           monitor > 0 ? "monitor " + std::to_string(monitor) : "the primary monitor");
}

std::unique_ptr<Source> MakeWindowSource(const std::string& title, bool cursor)
{
    WindowSearch search{LowerText(Wide(title)), nullptr, GetConsoleWindow()};
    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            auto* s = reinterpret_cast<WindowSearch*>(data);
            if (hwnd == s->own_console || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOTOWNER) != hwnd) {
                return TRUE;
            }
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            wchar_t text[512] = {};
            if (cloaked || GetWindowTextW(hwnd, text, 512) == 0) {
                return TRUE;
            }
            if (LowerText(text).find(s->needle) != std::wstring::npos) {
                s->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
        return std::make_unique<MissingSource>("no window title contains \"" + title + "\" (see `vcam list`)");
    }
    wchar_t text[512] = {};
    GetWindowTextW(search.found, text, 512);
    return std::make_unique<CaptureSource>(nullptr, search.found, cursor, "window \"" + Utf8(text) + "\"");
}

void ConfigureCapture(Source& source, ScaleMode scale, Rgb background)
{
    if (auto* capture = dynamic_cast<CaptureSource*>(&source)) {
        capture->scale = scale;
        capture->background = background;
    }
}

}  // namespace vcam::app
