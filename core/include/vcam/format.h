// Pixel formats, image views and plane geometry. Portable, no OS dependencies.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vcam {

// Values are part of the shared-memory protocol and the public SDK; never renumber.
enum class PixelFormat : uint32_t {
    Unknown = 0,
    BGRA = 1,   // bytes B,G,R,A per pixel (Media Foundation RGB32); alpha is ignored on input
    RGBA = 2,   // bytes R,G,B,A
    BGR24 = 3,  // bytes B,G,R (OpenCV order)
    RGB24 = 4,  // bytes R,G,B (PIL / numpy RGB order)
    Gray8 = 5,  // one byte of full-range luma per pixel
    NV12 = 6,   // Y plane, then interleaved U,V plane at half width and half height
    I420 = 7,   // Y plane, U plane, V plane; chroma at half width and half height
    YUY2 = 8,   // packed 4:2:2, bytes Y0 U Y1 V
    UYVY = 9,   // packed 4:2:2, bytes U Y0 V Y1
};

constexpr uint32_t kPixelFormatCount = 10;

// Values are part of the shared-memory protocol; never renumber.
enum class ColorSpace : uint32_t {
    Bt601Limited = 0,
    Bt709Limited = 1,
    Bt601Full = 2,
    Bt709Full = 3,
};

enum class ScaleMode : uint32_t {
    Fit = 0,      // keep aspect ratio, pad with the background colour
    Fill = 1,     // keep aspect ratio, crop the overflow
    Stretch = 2,  // ignore aspect ratio
};

// Largest frame dimension anything in vcam accepts.
constexpr uint32_t kMaxDimension = 8192;

bool IsValidFormat(uint32_t value);
bool IsValidColorSpace(uint32_t value);
bool IsYuv(PixelFormat format);
const char* FormatName(PixelFormat format);
// Parses names such as "nv12", "BGRA", "rgb24" (case-insensitive). Returns Unknown on failure.
PixelFormat ParseFormat(const char* name);

int PlaneCount(PixelFormat format);

// Bytes in one row of `plane` (without padding) and the number of rows, for a
// width x height image. Odd sizes round chroma up. Returns false for an unknown
// format, a bad plane index or a size outside 1..kMaxDimension.
bool PlaneGeometry(PixelFormat format, uint32_t width, uint32_t height, int plane,
                   uint32_t* row_bytes, uint32_t* rows);

// Tightly packed layout: planes back to back, each row padded to `pitch_align` bytes.
struct PlaneLayout {
    int count = 0;
    uint32_t offset[3] = {};
    uint32_t pitch[3] = {};
    uint64_t total_bytes = 0;
};
bool DefaultLayout(PixelFormat format, uint32_t width, uint32_t height, PlaneLayout* layout,
                   uint32_t pitch_align = 1);

// Read-only view of an image somebody else owns. Pitches may be negative
// (bottom-up images): plane[p] then points at the first (top) row.
struct ImageView {
    PixelFormat format = PixelFormat::Unknown;
    int width = 0;
    int height = 0;
    const uint8_t* plane[3] = {};
    int pitch[3] = {};
};

// Writable view of an image somebody else owns.
struct ImageSpan {
    PixelFormat format = PixelFormat::Unknown;
    int width = 0;
    int height = 0;
    uint8_t* plane[3] = {};
    int pitch[3] = {};

    ImageView view() const;
};

// Builds a view over a tightly packed buffer laid out by DefaultLayout(pitch_align = 1).
ImageSpan MakeSpan(PixelFormat format, int width, int height, uint8_t* data);
ImageView MakeView(PixelFormat format, int width, int height, const uint8_t* data);

// Checks that the view's pointers are set and every pitch covers its row.
bool IsUsable(const ImageView& view);
bool IsUsable(const ImageSpan& span);

}  // namespace vcam
