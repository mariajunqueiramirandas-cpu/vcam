// DirectShow media types: what the capture pin offers and how a type maps to
// the bytes of a sample.
#include "dshow.h"

#include <algorithm>
#include <cstring>

namespace vcam::win {

namespace {

// I420 and its alias IYUV (not in every SDK's uuids.h).
const GUID kSubtypeI420 = {0x30323449, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeIYUV = {0x56555949, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr DWORD FourCC(char a, char b, char c, char d)
{
    return static_cast<DWORD>(static_cast<uint8_t>(a)) | (static_cast<DWORD>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<DWORD>(static_cast<uint8_t>(c)) << 16) | (static_cast<DWORD>(static_cast<uint8_t>(d)) << 24);
}

constexpr int64_t kMinInterval = 10'000'000 / 240;
constexpr int64_t kMaxInterval = 10'000'000;
constexpr int64_t kDefaultInterval = 333333;

struct FormatInfo {
    PixelFormat format;
    const GUID* subtype;
    WORD bits;
    DWORD compression;
};

const FormatInfo* InfoFor(PixelFormat format)
{
    static const FormatInfo infos[] = {
        {PixelFormat::NV12, &MEDIASUBTYPE_NV12, 12, FourCC('N', 'V', '1', '2')},
        {PixelFormat::YUY2, &MEDIASUBTYPE_YUY2, 16, FourCC('Y', 'U', 'Y', '2')},
        {PixelFormat::I420, &kSubtypeI420, 12, FourCC('I', '4', '2', '0')},
        {PixelFormat::BGRA, &MEDIASUBTYPE_RGB32, 32, BI_RGB},
    };
    for (const FormatInfo& info : infos) {
        if (info.format == format) {
            return &info;
        }
    }
    return nullptr;
}

bool Planar(PixelFormat format)
{
    return format == PixelFormat::NV12 || format == PixelFormat::I420;
}

uint64_t ImageBytes(PixelFormat format, uint64_t stride, uint64_t rows)
{
    return Planar(format) ? stride * rows + 2 * (stride / 2) * (rows / 2) : stride * rows;
}

}  // namespace

// --- ownership --------------------------------------------------------------------------

void FreeMediaType(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat != 0 || mt.pbFormat) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE* mt)
{
    if (mt) {
        FreeMediaType(*mt);
        CoTaskMemFree(mt);
    }
}

HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE& src)
{
    *dst = src;
    if (src.cbFormat != 0 && src.pbFormat) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst->pbFormat) {
            dst->cbFormat = 0;
            dst->pUnk = nullptr;
            return E_OUTOFMEMORY;
        }
        memcpy(dst->pbFormat, src.pbFormat, src.cbFormat);
    } else {
        dst->cbFormat = 0;
        dst->pbFormat = nullptr;
    }
    if (dst->pUnk) {
        dst->pUnk->AddRef();
    }
    return S_OK;
}

MediaType::MediaType()
{
    memset(&mt_, 0, sizeof(mt_));
}

MediaType::MediaType(const AM_MEDIA_TYPE& mt)
{
    memset(&mt_, 0, sizeof(mt_));
    if (FAILED(CopyMediaType(&mt_, mt))) {
        memset(&mt_, 0, sizeof(mt_));
    }
}

MediaType::MediaType(const MediaType& other) : MediaType(other.mt_)
{
}

MediaType& MediaType::operator=(const MediaType& other)
{
    if (this != &other) {
        FreeMediaType(mt_);
        memset(&mt_, 0, sizeof(mt_));
        if (FAILED(CopyMediaType(&mt_, other.mt_))) {
            memset(&mt_, 0, sizeof(mt_));
        }
    }
    return *this;
}

MediaType::~MediaType()
{
    FreeMediaType(mt_);
}

AM_MEDIA_TYPE* MediaType::CreateCopy() const
{
    auto* copy = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (copy && FAILED(CopyMediaType(copy, mt_))) {
        CoTaskMemFree(copy);
        copy = nullptr;
    }
    return copy;
}

// --- meaning ----------------------------------------------------------------------------

bool ParseVideoType(const AM_MEDIA_TYPE& mt, VideoFormat* out)
{
    if (mt.majortype != MEDIATYPE_Video || mt.formattype != FORMAT_VideoInfo || !mt.pbFormat ||
        mt.cbFormat < sizeof(VIDEOINFOHEADER)) {
        return false;
    }
    VideoFormat f;
    if (mt.subtype == MEDIASUBTYPE_NV12) {
        f.format = PixelFormat::NV12;
    } else if (mt.subtype == MEDIASUBTYPE_YUY2) {
        f.format = PixelFormat::YUY2;
    } else if (mt.subtype == kSubtypeI420 || mt.subtype == kSubtypeIYUV) {
        f.format = PixelFormat::I420;
    } else if (mt.subtype == MEDIASUBTYPE_RGB32) {
        f.format = PixelFormat::BGRA;
    } else {
        return false;
    }
    f.subtype = mt.subtype;
    VIDEOINFOHEADER vih;
    memcpy(&vih, mt.pbFormat, sizeof(vih));
    const BITMAPINFOHEADER& bmi = vih.bmiHeader;
    if (bmi.biWidth <= 0 || bmi.biWidth > static_cast<LONG>(kMaxDimension) || bmi.biHeight == 0 ||
        bmi.biHeight < -static_cast<LONG>(kMaxDimension) || bmi.biHeight > static_cast<LONG>(kMaxDimension)) {
        return false;
    }
    const LONG rows = bmi.biHeight < 0 ? -bmi.biHeight : bmi.biHeight;
    f.width = static_cast<int>(bmi.biWidth);
    f.height = static_cast<int>(rows);
    // A renderer may ask for padded rows: the picture is rcTarget, the buffer
    // is biWidth wide. Only padding on the right is supported.
    const RECT& target = vih.rcTarget;
    if (target.right > target.left && target.bottom > target.top) {
        if (target.left != 0 || target.top != 0 || target.right > bmi.biWidth || target.bottom != rows) {
            return false;
        }
        f.width = static_cast<int>(target.right);
    }
    if (f.width < 16 || f.height < 16 || f.width > 4096 || f.height > 4096) {
        return false;
    }
    if (IsYuv(f.format) && ((f.width | f.height | static_cast<int>(bmi.biWidth)) & 1)) {
        return false;
    }
    const FormatInfo* info = InfoFor(f.format);
    f.stride = Planar(f.format) ? static_cast<int>(bmi.biWidth) : static_cast<int>(bmi.biWidth) * info->bits / 8;
    f.bottom_up = f.format == PixelFormat::BGRA && bmi.biHeight > 0;
    const uint64_t bytes = ImageBytes(f.format, static_cast<uint64_t>(f.stride), static_cast<uint64_t>(rows));
    if (bytes > 0x7fffffff) {
        return false;
    }
    f.image_bytes = static_cast<uint32_t>(bytes);
    f.interval = vih.AvgTimePerFrame > 0 ? std::clamp<int64_t>(vih.AvgTimePerFrame, kMinInterval, kMaxInterval)
                                         : kDefaultInterval;
    *out = f;
    return true;
}

bool SameVideo(const VideoFormat& a, const VideoFormat& b)
{
    return a.format == b.format && a.width == b.width && a.height == b.height && a.bottom_up == b.bottom_up;
}

MediaType MakeVideoType(PixelFormat format, uint32_t width, uint32_t height, uint32_t fps)
{
    const FormatInfo* info = InfoFor(format);
    if (!info || fps == 0) {
        return MediaType();
    }
    const uint64_t stride = Planar(format) ? width : static_cast<uint64_t>(width) * info->bits / 8;
    const uint64_t bytes = ImageBytes(format, stride, height);
    VIDEOINFOHEADER vih;
    memset(&vih, 0, sizeof(vih));
    vih.dwBitRate = static_cast<DWORD>(std::min<uint64_t>(bytes * 8 * fps, 0xffffffffu));
    vih.AvgTimePerFrame = 10'000'000 / static_cast<int64_t>(fps);
    BITMAPINFOHEADER& bmi = vih.bmiHeader;
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = static_cast<LONG>(width);
    bmi.biHeight = static_cast<LONG>(height);  // RGB: bottom-up, as DirectShow expects
    bmi.biPlanes = 1;
    bmi.biBitCount = info->bits;
    bmi.biCompression = info->compression;
    bmi.biSizeImage = static_cast<DWORD>(bytes);

    AM_MEDIA_TYPE mt;
    memset(&mt, 0, sizeof(mt));
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = *info->subtype;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = static_cast<ULONG>(bytes);
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(vih);
    mt.pbFormat = reinterpret_cast<BYTE*>(&vih);
    return MediaType(mt);  // copies the format block
}

std::vector<OfferedType> OfferedTypes(const Config& config)
{
    std::vector<OfferedType> types;
    for (const VideoMode& mode : StandardModes(config.max_width, config.max_height, config.allow_60fps)) {
        for (const PixelFormat format : {PixelFormat::NV12, PixelFormat::YUY2, PixelFormat::I420, PixelFormat::BGRA}) {
            types.push_back(OfferedType{mode, format});
        }
    }
    return types;
}

}  // namespace vcam::win
