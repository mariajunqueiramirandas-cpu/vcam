// Video files and webcams through the Media Foundation Source Reader.
#include "app.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace vcam::app {

namespace {

class MediaFoundationScope {
public:
    MediaFoundationScope()
    {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf_ = SUCCEEDED(MFStartup(MF_VERSION));
    }
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;
    ~MediaFoundationScope()
    {
        if (mf_) MFShutdown();
        if (com_) CoUninitialize();
    }

private:
    bool com_ = false, mf_ = false;
};

ColorSpace ColorSpaceOf(IMFMediaType* type, UINT32 height)
{
    UINT32 matrix = 0, range = 0;
    const bool has_matrix = SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix));
    const bool full = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)) && range == MFNominalRange_0_255;
    // Without a tag, HD content is BT.709 and SD BT.601 by convention.
    const bool bt709 = has_matrix ? matrix == MFVideoTransferMatrix_BT709 : height >= 720;
    if (bt709) {
        return full ? ColorSpace::Bt709Full : ColorSpace::Bt709Limited;
    }
    return full ? ColorSpace::Bt601Full : ColorSpace::Bt601Limited;
}

// Common part: NV12 output from a Source Reader, frames exposed in place.
class ReaderSource : public Source {
public:
    ~ReaderSource() override { Unlock(); }
    bool SelfPaced() const override { return true; }

protected:
    HRESULT ConfigureOutput()
    {
        ComPtr<IMFMediaType> type;
        HRESULT hr = MFCreateMediaType(&type);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
        if (SUCCEEDED(hr)) hr = ReadFormat();
        return hr;
    }

    HRESULT ReadFormat()
    {
        ComPtr<IMFMediaType> current;
        HRESULT hr = reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
        if (FAILED(hr)) {
            return hr;
        }
        hr = MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width_, &height_);
        if (FAILED(hr) || width_ < 2 || height_ < 2) {
            return FAILED(hr) ? hr : MF_E_INVALIDMEDIATYPE;
        }
        UINT32 stride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
            stride_ = static_cast<LONG>(stride);
        } else {
            stride_ = static_cast<LONG>(width_);
        }
        UINT32 size = 0;
        has_aperture_ = SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture_),
                                                   sizeof(aperture_), &size)) &&
                        size == sizeof(aperture_);
        UINT32 num = 0, den = 0;
        frame_duration_ = SUCCEEDED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &num, &den)) && num && den
                              ? static_cast<LONGLONG>(10'000'000.0 * den / num)
                              : 333333;
        colorspace_ = ColorSpaceOf(current.Get(), height_);
        return S_OK;
    }

    // Reads the next frame and exposes it until the following call.
    bool ReadFrame(Frame* frame, bool* ended, LONGLONG* timestamp)
    {
        Unlock();
        DWORD stream = 0, flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream,
                                               &flags, &time, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            PrintError("\nreading %s failed: %s\n", name(), HresultText(FAILED(hr) ? hr : E_FAIL).c_str());
            *ended = true;
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ReadFormat();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            *ended = true;
            return false;
        }
        if (!sample) {
            return false;  // a gap in the stream
        }
        LONGLONG duration = 0;
        if (SUCCEEDED(sample->GetSampleDuration(&duration)) && duration > 0) {
            frame_duration_ = duration;
        }
        DWORD buffers = 0;
        sample->GetBufferCount(&buffers);
        if (FAILED(buffers > 1 ? sample->ConvertToContiguousBuffer(&buffer_) : sample->GetBufferByIndex(0, &buffer_))) {
            return false;
        }
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer_.As(&buffer2d_)) && SUCCEEDED(buffer2d_->Lock2D(&scan0, &pitch))) {
            locked2d_ = true;
        } else {
            buffer2d_.Reset();
            DWORD max_length = 0, length = 0;
            if (FAILED(buffer_->Lock(&scan0, &max_length, &length))) {
                return false;
            }
            locked_ = true;
            pitch = stride_;
            if (static_cast<uint64_t>(length) < static_cast<uint64_t>(pitch) * height_ * 3 / 2) {
                return false;
            }
        }
        if (pitch <= 0) {
            return false;
        }
        Frame f;
        f.format = PixelFormat::NV12;
        f.width = static_cast<int>(width_);
        f.height = static_cast<int>(height_);
        f.planes[0] = scan0;
        f.planes[1] = scan0 + static_cast<ptrdiff_t>(pitch) * height_;
        f.pitches[0] = f.pitches[1] = pitch;
        f.colorspace = colorspace_;
        // Decoders pad (1080p H.264 decodes to 1088 lines): show only the display area.
        if (has_aperture_) {
            const int x = aperture_.OffsetX.value & ~1, y = aperture_.OffsetY.value & ~1;
            const int w = static_cast<int>(aperture_.Area.cx), h = static_cast<int>(aperture_.Area.cy);
            if (x >= 0 && y >= 0 && w >= 2 && h >= 2 && x + w <= f.width && y + h <= f.height) {
                f.planes[0] += static_cast<ptrdiff_t>(y) * pitch + x;
                f.planes[1] += static_cast<ptrdiff_t>(y / 2) * pitch + x;
                f.width = w;
                f.height = h;
            }
        }
        *frame = f;
        *timestamp = time;
        sample_ = sample;
        return true;
    }

    void Unlock()
    {
        if (locked2d_) {
            buffer2d_->Unlock2D();
        } else if (locked_) {
            buffer_->Unlock();
        }
        locked2d_ = locked_ = false;
        buffer2d_.Reset();
        buffer_.Reset();
        sample_.Reset();
    }

    MediaFoundationScope mf_;
    ComPtr<IMFSourceReader> reader_;
    UINT32 width_ = 0, height_ = 0;
    LONG stride_ = 0;
    MFVideoArea aperture_{};
    bool has_aperture_ = false;
    ColorSpace colorspace_ = ColorSpace::Bt601Limited;
    LONGLONG frame_duration_ = 333333;

private:
    ComPtr<IMFSample> sample_;
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMF2DBuffer> buffer2d_;
    bool locked2d_ = false, locked_ = false;
};

HRESULT ReaderAttributes(IMFAttributes** attributes)
{
    ComPtr<IMFAttributes> a;
    HRESULT hr = MFCreateAttributes(&a, 2);
    // Decoding, colour conversion and deinterlacing inside the reader.
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) *attributes = a.Detach();
    return hr;
}

// --- video file -------------------------------------------------------------------------------

class VideoSource final : public ReaderSource {
public:
    VideoSource(std::wstring path, bool loop) : path_(std::move(path)), loop_(loop) {}
    const char* name() const override { return "video file"; }

    bool Start(std::string* error) override
    {
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = ReaderAttributes(&attributes);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path_.c_str(), attributes.Get(), &reader_);
        if (SUCCEEDED(hr)) hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (SUCCEEDED(hr)) hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        if (SUCCEEDED(hr)) hr = ConfigureOutput();
        if (FAILED(hr)) {
            *error = "cannot play " + Utf8(path_) + ": " + HresultText(hr);
            return false;
        }
        return true;
    }

    bool Next(const Target&, int, Frame* frame, bool* ended) override
    {
        LONGLONG ts = 0;
        if (!ReadFrame(frame, ended, &ts)) {
            if (*ended && loop_ && frames_ > 0) {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = 0;
                if (SUCCEEDED(reader_->SetCurrentPosition(GUID_NULL, position))) {
                    offset_ += last_ts_ + frame_duration_;
                    *ended = false;
                }
            }
            return false;
        }
        frames_++;
        last_ts_ = ts;
        // Present at the file's own pace.
        const int64_t now = win::NowTicks();
        const int64_t media_time = offset_ + ts;
        if (!started_ || now - (base_ + media_time) > shm::kTicksPerSecond / 2 ||
            (base_ + media_time) - now > shm::kTicksPerSecond) {
            base_ = now - media_time;
            started_ = true;
        }
        const int64_t due = base_ + media_time;
        while (!stopping() && win::NowTicks() < due) {
            const int64_t left = due - win::NowTicks();
            Sleep(left > 20000 ? static_cast<DWORD>(left / 10000 - 1) : 0);
        }
        return true;
    }

    void Pause() override { paused_at_ = win::NowTicks(); }
    void Resume() override
    {
        if (paused_at_) {
            base_ += win::NowTicks() - paused_at_;  // continue where it stopped
            paused_at_ = 0;
        }
    }

private:
    std::wstring path_;
    bool loop_;
    bool started_ = false;
    int64_t base_ = 0, paused_at_ = 0;
    LONGLONG offset_ = 0, last_ts_ = 0;
    uint64_t frames_ = 0;
};

// --- webcam ------------------------------------------------------------------------------------------

class WebcamSource final : public ReaderSource {
public:
    explicit WebcamSource(std::string selector) : selector_(std::move(selector)) {}
    const char* name() const override { return "webcam"; }

    bool Start(std::string* error) override
    {
        const std::vector<CameraInfo> all = EnumerateCameras();
        std::vector<CameraInfo> cameras;
        for (const auto& c : all) {
            if (!c.is_vcam) {
                cameras.push_back(c);
            }
        }
        if (cameras.empty()) {
            *error = "no camera found (vcam's own cameras cannot be used as a source)";
            return false;
        }
        int index = 0;
        if (!selector_.empty() && ParseInt(selector_, &index)) {
            if (index < 1 || index > static_cast<int>(cameras.size())) {
                *error = "there is no camera " + selector_ + " (see `vcam list`)";
                return false;
            }
            device_ = cameras[static_cast<size_t>(index - 1)];
        } else if (!selector_.empty()) {
            std::wstring needle = Wide(selector_);
            for (auto& c : needle) c = static_cast<wchar_t>(towlower(c));
            bool found = false;
            for (const auto& c : cameras) {
                std::wstring hay = c.name;
                for (auto& ch : hay) ch = static_cast<wchar_t>(towlower(ch));
                if (hay.find(needle) != std::wstring::npos) {
                    device_ = c;
                    found = true;
                    break;
                }
            }
            if (!found) {
                *error = "no camera matches \"" + selector_ + "\" (see `vcam list`)";
                return false;
            }
        } else {
            device_ = cameras[0];
        }
        const HRESULT hr = Open();
        if (FAILED(hr)) {
            *error = "cannot open " + Utf8(device_.name) + ": " + HresultText(hr);
            return false;
        }
        Print("Using %s at %ux%u.\n", Utf8(device_.name).c_str(), width_, height_);
        return true;
    }

    bool Next(const Target&, int, Frame* frame, bool* ended) override
    {
        if (!reader_ && FAILED(Open())) {
            Sleep(500);
            return false;
        }
        LONGLONG ts = 0;
        return ReadFrame(frame, ended, &ts);
    }

    // Release the camera (and its light) while nobody watches.
    void Pause() override { Close(); }
    void Resume() override {}

    ~WebcamSource() override { Close(); }

private:
    HRESULT Open()
    {
        ComPtr<IMFAttributes> device;
        HRESULT hr = MFCreateAttributes(&device, 2);
        if (SUCCEEDED(hr)) hr = device->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = device->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, device_.symbolic_link.c_str());
        if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(device.Get(), &source_);
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(hr)) hr = ReaderAttributes(&attributes);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source_.Get(), attributes.Get(), &reader_);
        if (SUCCEEDED(hr)) hr = ChooseMode();
        if (SUCCEEDED(hr)) hr = ConfigureOutput();
        if (FAILED(hr)) {
            Close();
        }
        return hr;
    }

    void Close()
    {
        Unlock();
        reader_.Reset();
        if (source_) {
            source_->Shutdown();
            source_.Reset();
        }
    }

    // Prefers 1080p (or the nearest size) at 30 fps or more.
    HRESULT ChooseMode()
    {
        ComPtr<IMFMediaType> best;
        double best_score = -1e9;
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> type;
            if (FAILED(reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type))) {
                break;
            }
            UINT32 w = 0, h = 0, num = 0, den = 1;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
            if (!w || !h || !den) {
                continue;
            }
            const double fps = static_cast<double>(num) / den;
            double score = -std::fabs(std::log2(static_cast<double>(w) * h / (1920.0 * 1080.0))) * 10;
            score += fps >= 29 ? 50 : fps * 1.5;
            GUID subtype = GUID_NULL;
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2) {
                score += 1;  // no decoding needed
            }
            if (score > best_score) {
                best_score = score;
                best = type;
            }
        }
        return best ? reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                                   best.Get())
                    : MF_E_INVALIDMEDIATYPE;
    }

    std::string selector_;
    CameraInfo device_;
    ComPtr<IMFMediaSource> source_;
};

}  // namespace

std::unique_ptr<Source> MakeVideoSource(const std::wstring& path, bool loop)
{
    return std::make_unique<VideoSource>(path, loop);
}

std::unique_ptr<Source> MakeWebcamSource(const std::string& selector)
{
    return std::make_unique<WebcamSource>(selector);
}

}  // namespace vcam::app
