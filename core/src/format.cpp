#include "vcam/format.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace vcam {

bool IsValidFormat(uint32_t value)
{
    return value >= 1 && value < kPixelFormatCount;
}

bool IsValidColorSpace(uint32_t value)
{
    return value <= static_cast<uint32_t>(ColorSpace::Bt709Full);
}

bool IsYuv(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::I420:
    case PixelFormat::YUY2:
    case PixelFormat::UYVY:
        return true;
    default:
        return false;
    }
}

const char* FormatName(PixelFormat format)
{
    switch (format) {
    case PixelFormat::BGRA: return "BGRA";
    case PixelFormat::RGBA: return "RGBA";
    case PixelFormat::BGR24: return "BGR24";
    case PixelFormat::RGB24: return "RGB24";
    case PixelFormat::Gray8: return "GRAY8";
    case PixelFormat::NV12: return "NV12";
    case PixelFormat::I420: return "I420";
    case PixelFormat::YUY2: return "YUY2";
    case PixelFormat::UYVY: return "UYVY";
    default: return "unknown";
    }
}

PixelFormat ParseFormat(const char* name)
{
    if (!name) {
        return PixelFormat::Unknown;
    }
    char lower[16] = {};
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof(lower)) {
        return PixelFormat::Unknown;
    }
    for (size_t i = 0; i < n; i++) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
    }
    struct Alias {
        const char* name;
        PixelFormat format;
    };
    static const Alias aliases[] = {
        {"bgra", PixelFormat::BGRA},  {"bgrx", PixelFormat::BGRA},  {"rgb32", PixelFormat::BGRA},
        {"rgba", PixelFormat::RGBA},  {"bgr24", PixelFormat::BGR24}, {"bgr", PixelFormat::BGR24},
        {"rgb24", PixelFormat::RGB24}, {"rgb", PixelFormat::RGB24},  {"gray8", PixelFormat::Gray8},
        {"gray", PixelFormat::Gray8}, {"grey", PixelFormat::Gray8},  {"y8", PixelFormat::Gray8},
        {"nv12", PixelFormat::NV12},  {"i420", PixelFormat::I420},  {"iyuv", PixelFormat::I420},
        {"yuv420p", PixelFormat::I420}, {"yuy2", PixelFormat::YUY2}, {"yuyv", PixelFormat::YUY2},
        {"uyvy", PixelFormat::UYVY},
    };
    for (const Alias& a : aliases) {
        if (strcmp(lower, a.name) == 0) {
            return a.format;
        }
    }
    return PixelFormat::Unknown;
}

int PlaneCount(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12: return 2;
    case PixelFormat::I420: return 3;
    case PixelFormat::Unknown: return 0;
    default: return IsValidFormat(static_cast<uint32_t>(format)) ? 1 : 0;
    }
}

bool PlaneGeometry(PixelFormat format, uint32_t width, uint32_t height, int plane,
                   uint32_t* row_bytes, uint32_t* rows)
{
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
        plane < 0 || plane >= PlaneCount(format)) {
        return false;
    }
    const uint32_t half_w = (width + 1) / 2;
    const uint32_t half_h = (height + 1) / 2;
    uint32_t rb = 0;
    uint32_t r = height;
    switch (format) {
    case PixelFormat::BGRA:
    case PixelFormat::RGBA: rb = width * 4; break;
    case PixelFormat::BGR24:
    case PixelFormat::RGB24: rb = width * 3; break;
    case PixelFormat::Gray8: rb = width; break;
    case PixelFormat::YUY2:
    case PixelFormat::UYVY: rb = half_w * 4; break;
    case PixelFormat::NV12:
        rb = plane == 0 ? width : half_w * 2;
        r = plane == 0 ? height : half_h;
        break;
    case PixelFormat::I420:
        rb = plane == 0 ? width : half_w;
        r = plane == 0 ? height : half_h;
        break;
    default: return false;
    }
    *row_bytes = rb;
    *rows = r;
    return true;
}

bool DefaultLayout(PixelFormat format, uint32_t width, uint32_t height, PlaneLayout* layout,
                   uint32_t pitch_align)
{
    if (pitch_align == 0 || (pitch_align & (pitch_align - 1)) != 0) {
        return false;
    }
    PlaneLayout out;
    out.count = PlaneCount(format);
    if (out.count == 0) {
        return false;
    }
    uint64_t offset = 0;
    for (int p = 0; p < out.count; p++) {
        uint32_t rb = 0, rows = 0;
        if (!PlaneGeometry(format, width, height, p, &rb, &rows)) {
            return false;
        }
        const uint32_t pitch = (rb + pitch_align - 1) & ~(pitch_align - 1);
        out.offset[p] = static_cast<uint32_t>(offset);
        out.pitch[p] = pitch;
        offset += static_cast<uint64_t>(pitch) * rows;
    }
    out.total_bytes = offset;
    *layout = out;
    return true;
}

ImageView ImageSpan::view() const
{
    ImageView v;
    v.format = format;
    v.width = width;
    v.height = height;
    for (int p = 0; p < 3; p++) {
        v.plane[p] = plane[p];
        v.pitch[p] = pitch[p];
    }
    return v;
}

ImageSpan MakeSpan(PixelFormat format, int width, int height, uint8_t* data)
{
    ImageSpan s;
    PlaneLayout layout;
    if (width <= 0 || height <= 0 || !data ||
        !DefaultLayout(format, static_cast<uint32_t>(width), static_cast<uint32_t>(height), &layout)) {
        return s;
    }
    s.format = format;
    s.width = width;
    s.height = height;
    for (int p = 0; p < layout.count; p++) {
        s.plane[p] = data + layout.offset[p];
        s.pitch[p] = static_cast<int>(layout.pitch[p]);
    }
    return s;
}

ImageView MakeView(PixelFormat format, int width, int height, const uint8_t* data)
{
    return MakeSpan(format, width, height, const_cast<uint8_t*>(data)).view();
}

namespace {

template <typename View>
bool CheckUsable(const View& v)
{
    if (v.width <= 0 || v.height <= 0) {
        return false;
    }
    const int planes = PlaneCount(v.format);
    if (planes == 0) {
        return false;
    }
    for (int p = 0; p < planes; p++) {
        uint32_t rb = 0, rows = 0;
        if (!PlaneGeometry(v.format, static_cast<uint32_t>(v.width), static_cast<uint32_t>(v.height), p,
                           &rb, &rows)) {
            return false;
        }
        if (!v.plane[p] || static_cast<uint32_t>(std::abs(v.pitch[p])) < rb) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool IsUsable(const ImageView& view)
{
    return CheckUsable(view);
}

bool IsUsable(const ImageSpan& span)
{
    return CheckUsable(span);
}

}  // namespace vcam
