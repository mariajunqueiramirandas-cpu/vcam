// Conversion pipeline: decode the source into a canonical form (BGRA for RGB
// formats, I420 for YUV formats), resample it with a separable tent filter into
// the destination rectangle, then encode into the destination format. Scaling
// always happens in the source's colour family and colour conversion at the
// destination size, so the expensive per-pixel work is done once per output pixel.
#include "vcam/convert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "simd.h"

namespace vcam {
namespace {

inline const uint8_t* RowPtr(const uint8_t* base, int pitch, int y)
{
    return base + static_cast<ptrdiff_t>(pitch) * y;
}

inline uint8_t* RowPtr(uint8_t* base, int pitch, int y)
{
    return base + static_cast<ptrdiff_t>(pitch) * y;
}

inline uint8_t Clamp255(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Fixed-point colour matrices: RGB -> YUV with 15 fractional bits (so the
// coefficients fit the SIMD kernels' 16-bit multiplies), YUV -> RGB with 16.
struct Coeffs {
    int32_t yr, yg, yb, y_add;
    int32_t ur, ug, ub, vr, vg, vb;
    int32_t y_base, ky, rv, gu, gv, bu;
    simd::YuvCoeffs15 packed;
};

Coeffs MakeCoeffs(ColorSpace cs)
{
    const bool bt709 = cs == ColorSpace::Bt709Limited || cs == ColorSpace::Bt709Full;
    const bool limited = cs == ColorSpace::Bt601Limited || cs == ColorSpace::Bt709Limited;
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const double sy = limited ? 219.0 / 255.0 : 1.0;
    const double sc = limited ? 224.0 / 255.0 : 1.0;
    const double one = 65536.0;
    const double one15 = 32768.0;
    const double cb = 2.0 * (1.0 - kb);
    const double cr = 2.0 * (1.0 - kr);

    Coeffs c{};
    c.yr = static_cast<int32_t>(std::lround(sy * kr * one15));
    c.yg = static_cast<int32_t>(std::lround(sy * kg * one15));
    // Make the luma weights sum exactly to the scale so grey stays grey and white hits 235/255.
    c.yb = static_cast<int32_t>(std::lround(sy * one15)) - c.yr - c.yg;
    c.y_add = ((limited ? 16 : 0) << 15) + (1 << 14);
    c.ur = static_cast<int32_t>(std::lround(-sc * kr / cb * one15));
    c.ug = static_cast<int32_t>(std::lround(-sc * kg / cb * one15));
    c.ub = -(c.ur + c.ug);  // chroma weights sum to zero: grey has no chroma
    c.vg = static_cast<int32_t>(std::lround(-sc * kg / cr * one15));
    c.vb = static_cast<int32_t>(std::lround(-sc * kb / cr * one15));
    c.vr = -(c.vg + c.vb);
    c.packed = {static_cast<int16_t>(c.yb), static_cast<int16_t>(c.yg), static_cast<int16_t>(c.yr),
                static_cast<int16_t>(c.ub), static_cast<int16_t>(c.ug), static_cast<int16_t>(c.ur),
                static_cast<int16_t>(c.vb), static_cast<int16_t>(c.vg), static_cast<int16_t>(c.vr), c.y_add};

    c.y_base = limited ? 16 : 0;
    c.ky = static_cast<int32_t>(std::lround(one / sy));
    c.rv = static_cast<int32_t>(std::lround(cr / sc * one));
    c.bu = static_cast<int32_t>(std::lround(cb / sc * one));
    c.gu = static_cast<int32_t>(std::lround(cb * kb / kg / sc * one));
    c.gv = static_cast<int32_t>(std::lround(cr * kr / kg / sc * one));
    return c;
}

const Coeffs& GetCoeffs(ColorSpace cs)
{
    static const Coeffs table[4] = {
        MakeCoeffs(ColorSpace::Bt601Limited),
        MakeCoeffs(ColorSpace::Bt709Limited),
        MakeCoeffs(ColorSpace::Bt601Full),
        MakeCoeffs(ColorSpace::Bt709Full),
    };
    return table[static_cast<uint32_t>(cs) & 3];
}

inline uint8_t LumaOf(const Coeffs& c, int r, int g, int b)
{
    return Clamp255((c.yr * r + c.yg * g + c.yb * b + c.y_add) >> 15);
}

// Chroma from the sum of 1 << shift pixels.
inline uint8_t ChromaU(const Coeffs& c, int rs, int gs, int bs, int shift)
{
    return Clamp255((c.ur * rs + c.ug * gs + c.ub * bs + (128 << (15 + shift)) + (1 << (14 + shift))) >>
                    (15 + shift));
}

inline uint8_t ChromaV(const Coeffs& c, int rs, int gs, int bs, int shift)
{
    return Clamp255((c.vr * rs + c.vg * gs + c.vb * bs + (128 << (15 + shift)) + (1 << (14 + shift))) >>
                    (15 + shift));
}

inline void YuvPixelToRgb(const Coeffs& c, int y, int u, int v, uint8_t* r, uint8_t* g, uint8_t* b)
{
    const int yy = (y - c.y_base) * c.ky + 32768;
    const int d = u - 128;
    const int e = v - 128;
    *r = Clamp255((yy + c.rv * e) >> 16);
    *g = Clamp255((yy - c.gu * d - c.gv * e) >> 16);
    *b = Clamp255((yy + c.bu * d) >> 16);
}

int RgbBytesPerPixel(PixelFormat f)
{
    switch (f) {
    case PixelFormat::BGRA:
    case PixelFormat::RGBA: return 4;
    case PixelFormat::BGR24:
    case PixelFormat::RGB24: return 3;
    case PixelFormat::Gray8: return 1;
    default: return 0;
    }
}

// Sub-rectangle of a span. x and y must be even for subsampled formats.
ImageSpan SubSpan(const ImageSpan& s, int x, int y, int w, int h)
{
    ImageSpan r = s;
    r.width = w;
    r.height = h;
    switch (s.format) {
    case PixelFormat::NV12:
        r.plane[0] = RowPtr(s.plane[0], s.pitch[0], y) + x;
        r.plane[1] = RowPtr(s.plane[1], s.pitch[1], y / 2) + (x / 2) * 2;
        break;
    case PixelFormat::I420:
        r.plane[0] = RowPtr(s.plane[0], s.pitch[0], y) + x;
        r.plane[1] = RowPtr(s.plane[1], s.pitch[1], y / 2) + x / 2;
        r.plane[2] = RowPtr(s.plane[2], s.pitch[2], y / 2) + x / 2;
        break;
    case PixelFormat::YUY2:
    case PixelFormat::UYVY: r.plane[0] = RowPtr(s.plane[0], s.pitch[0], y) + (x / 2) * 4; break;
    default: r.plane[0] = RowPtr(s.plane[0], s.pitch[0], y) + x * RgbBytesPerPixel(s.format); break;
    }
    return r;
}

void CopyImage(const ImageView& s, const ImageSpan& d)
{
    for (int p = 0; p < PlaneCount(s.format); p++) {
        uint32_t rb = 0, rows = 0;
        PlaneGeometry(s.format, static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height), p, &rb, &rows);
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* out = RowPtr(d.plane[p], d.pitch[p], static_cast<int>(y));
            memcpy(out, RowPtr(s.plane[p], s.pitch[p], static_cast<int>(y)), rb);
            if (s.format == PixelFormat::BGRA || s.format == PixelFormat::RGBA) {
                for (uint32_t x = 3; x < rb; x += 4) {
                    out[x] = 255;
                }
            }
        }
    }
}

// --- decode to canonical -----------------------------------------------------

void RgbFamilyToBgra(const ImageView& s, uint8_t* dst, int dpitch)
{
    const int w = s.width;
    for (int y = 0; y < s.height; y++) {
        const uint8_t* in = RowPtr(s.plane[0], s.pitch[0], y);
        uint8_t* out = dst + static_cast<ptrdiff_t>(dpitch) * y;
        switch (s.format) {
        case PixelFormat::BGRA: memcpy(out, in, static_cast<size_t>(w) * 4); break;
        case PixelFormat::RGBA:
            for (int x = 0; x < w; x++, in += 4, out += 4) {
                out[0] = in[2], out[1] = in[1], out[2] = in[0], out[3] = 255;
            }
            break;
        case PixelFormat::BGR24:
            for (int x = 0; x < w; x++, in += 3, out += 4) {
                out[0] = in[0], out[1] = in[1], out[2] = in[2], out[3] = 255;
            }
            break;
        case PixelFormat::RGB24:
            for (int x = 0; x < w; x++, in += 3, out += 4) {
                out[0] = in[2], out[1] = in[1], out[2] = in[0], out[3] = 255;
            }
            break;
        case PixelFormat::Gray8:
            for (int x = 0; x < w; x++, in++, out += 4) {
                out[0] = out[1] = out[2] = *in, out[3] = 255;
            }
            break;
        default: break;
        }
    }
}

// NV12 / YUY2 / UYVY into I420 planes. For NV12 only the chroma is written
// (the caller references the source Y plane directly).
void YuvToI420(const ImageView& s, const ImageSpan& d)
{
    const int w = s.width, h = s.height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (s.format == PixelFormat::NV12) {
        for (int y = 0; y < ch; y++) {
            const uint8_t* uv = RowPtr(s.plane[1], s.pitch[1], y);
            uint8_t* u = RowPtr(d.plane[1], d.pitch[1], y);
            uint8_t* v = RowPtr(d.plane[2], d.pitch[2], y);
            for (int x = 0; x < cw; x++) {
                u[x] = uv[2 * x];
                v[x] = uv[2 * x + 1];
            }
        }
        return;
    }
    // Packed 4:2:2: byte offsets of Y0, U, Y1, V inside a macropixel.
    const bool uyvy = s.format == PixelFormat::UYVY;
    const int oy0 = uyvy ? 1 : 0, ou = uyvy ? 0 : 1, oy1 = uyvy ? 3 : 2, ov = uyvy ? 2 : 3;
    for (int y = 0; y < h; y++) {
        const uint8_t* in = RowPtr(s.plane[0], s.pitch[0], y);
        uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
        for (int x = 0; x + 1 < w; x += 2, in += 4) {
            out[x] = in[oy0];
            out[x + 1] = in[oy1];
        }
        if (w & 1) {
            out[w - 1] = in[oy0];
        }
    }
    for (int cy = 0; cy < ch; cy++) {
        const uint8_t* r0 = RowPtr(s.plane[0], s.pitch[0], 2 * cy);
        const uint8_t* r1 = RowPtr(s.plane[0], s.pitch[0], std::min(2 * cy + 1, h - 1));
        uint8_t* u = RowPtr(d.plane[1], d.pitch[1], cy);
        uint8_t* v = RowPtr(d.plane[2], d.pitch[2], cy);
        for (int x = 0; x < cw; x++) {
            u[x] = static_cast<uint8_t>((r0[4 * x + ou] + r1[4 * x + ou] + 1) >> 1);
            v[x] = static_cast<uint8_t>((r0[4 * x + ov] + r1[4 * x + ov] + 1) >> 1);
        }
    }
}

// --- encode from canonical BGRA ----------------------------------------------

void BgraToRgbFamily(const ImageView& s, const ImageSpan& d)
{
    const int w = s.width;
    for (int y = 0; y < s.height; y++) {
        const uint8_t* in = RowPtr(s.plane[0], s.pitch[0], y);
        uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
        switch (d.format) {
        case PixelFormat::BGRA:
            for (int x = 0; x < w; x++, in += 4, out += 4) {
                out[0] = in[0], out[1] = in[1], out[2] = in[2], out[3] = 255;
            }
            break;
        case PixelFormat::RGBA:
            for (int x = 0; x < w; x++, in += 4, out += 4) {
                out[0] = in[2], out[1] = in[1], out[2] = in[0], out[3] = 255;
            }
            break;
        case PixelFormat::BGR24:
            for (int x = 0; x < w; x++, in += 4, out += 3) {
                out[0] = in[0], out[1] = in[1], out[2] = in[2];
            }
            break;
        case PixelFormat::RGB24:
            for (int x = 0; x < w; x++, in += 4, out += 3) {
                out[0] = in[2], out[1] = in[1], out[2] = in[0];
            }
            break;
        case PixelFormat::Gray8:
            // Full-range BT.601 luma.
            for (int x = 0; x < w; x++, in += 4) {
                out[x] = static_cast<uint8_t>((7471 * in[0] + 38470 * in[1] + 19595 * in[2] + 32768) >> 16);
            }
            break;
        default: break;
        }
    }
}

void BgraToYuv420(const ImageView& s, const ImageSpan& d, const Coeffs& c)
{
    const bool nv12 = d.format == PixelFormat::NV12;
    const int w = s.width, h = s.height;
    for (int y = 0; y < h; y += 2) {
        const bool two_rows = y + 1 < h;
        const uint8_t* r0 = RowPtr(s.plane[0], s.pitch[0], y);
        const uint8_t* r1 = two_rows ? RowPtr(s.plane[0], s.pitch[0], y + 1) : r0;
        uint8_t* y0 = RowPtr(d.plane[0], d.pitch[0], y);
        uint8_t* y1 = two_rows ? RowPtr(d.plane[0], d.pitch[0], y + 1) : nullptr;
        uint8_t* uv = RowPtr(d.plane[1], d.pitch[1], y / 2);
        uint8_t* vv = nv12 ? nullptr : RowPtr(d.plane[2], d.pitch[2], y / 2);
        int x = 0;
        if (simd::Available()) {
            x = simd::BgraToYuv420Rows(r0, r1, w, y0, y1, uv, vv, c.packed);
        }
        for (; x + 1 < w; x += 2) {
            const uint8_t* a = r0 + 4 * x;
            const uint8_t* b = r1 + 4 * x;
            y0[x] = LumaOf(c, a[2], a[1], a[0]);
            y0[x + 1] = LumaOf(c, a[6], a[5], a[4]);
            if (two_rows) {
                y1[x] = LumaOf(c, b[2], b[1], b[0]);
                y1[x + 1] = LumaOf(c, b[6], b[5], b[4]);
            }
            const int rs = a[2] + a[6] + b[2] + b[6];
            const int gs = a[1] + a[5] + b[1] + b[5];
            const int bs = a[0] + a[4] + b[0] + b[4];
            if (nv12) {
                uv[x] = ChromaU(c, rs, gs, bs, 2);
                uv[x + 1] = ChromaV(c, rs, gs, bs, 2);
            } else {
                uv[x / 2] = ChromaU(c, rs, gs, bs, 2);
                vv[x / 2] = ChromaV(c, rs, gs, bs, 2);
            }
        }
        if (x < w) {  // odd width: last column
            const uint8_t* a = r0 + 4 * x;
            const uint8_t* b = r1 + 4 * x;
            y0[x] = LumaOf(c, a[2], a[1], a[0]);
            if (two_rows) {
                y1[x] = LumaOf(c, b[2], b[1], b[0]);
            }
            const int rs = a[2] + b[2], gs = a[1] + b[1], bs = a[0] + b[0];
            if (nv12) {
                uv[x] = ChromaU(c, rs, gs, bs, 1);
                uv[x + 1] = ChromaV(c, rs, gs, bs, 1);
            } else {
                uv[x / 2] = ChromaU(c, rs, gs, bs, 1);
                vv[x / 2] = ChromaV(c, rs, gs, bs, 1);
            }
        }
    }
}

void BgraToPacked422(const ImageView& s, const ImageSpan& d, const Coeffs& c)
{
    const bool uyvy = d.format == PixelFormat::UYVY;
    const int oy0 = uyvy ? 1 : 0, ou = uyvy ? 0 : 1, oy1 = uyvy ? 3 : 2, ov = uyvy ? 2 : 3;
    const int w = s.width;
    for (int y = 0; y < s.height; y++) {
        const uint8_t* in = RowPtr(s.plane[0], s.pitch[0], y);
        uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
        int x = 0;
        if (simd::Available()) {
            x = simd::BgraToPacked422Row(in, w, out, uyvy, c.packed);
            in += 4 * x;
            out += 2 * x;
        }
        for (; x + 1 < w; x += 2, in += 8, out += 4) {
            out[oy0] = LumaOf(c, in[2], in[1], in[0]);
            out[oy1] = LumaOf(c, in[6], in[5], in[4]);
            const int rs = in[2] + in[6], gs = in[1] + in[5], bs = in[0] + in[4];
            out[ou] = ChromaU(c, rs, gs, bs, 1);
            out[ov] = ChromaV(c, rs, gs, bs, 1);
        }
        if (x < w) {
            out[oy0] = out[oy1] = LumaOf(c, in[2], in[1], in[0]);
            out[ou] = ChromaU(c, in[2], in[1], in[0], 0);
            out[ov] = ChromaV(c, in[2], in[1], in[0], 0);
        }
    }
}

// --- encode from canonical I420 ----------------------------------------------

void I420ToYuv(const ImageView& s, const ImageSpan& d)
{
    const int w = s.width, h = s.height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    switch (d.format) {
    case PixelFormat::I420:
    case PixelFormat::NV12:
        for (int y = 0; y < h; y++) {
            memcpy(RowPtr(d.plane[0], d.pitch[0], y), RowPtr(s.plane[0], s.pitch[0], y), static_cast<size_t>(w));
        }
        for (int y = 0; y < ch; y++) {
            const uint8_t* u = RowPtr(s.plane[1], s.pitch[1], y);
            const uint8_t* v = RowPtr(s.plane[2], s.pitch[2], y);
            if (d.format == PixelFormat::I420) {
                memcpy(RowPtr(d.plane[1], d.pitch[1], y), u, static_cast<size_t>(cw));
                memcpy(RowPtr(d.plane[2], d.pitch[2], y), v, static_cast<size_t>(cw));
            } else {
                uint8_t* uv = RowPtr(d.plane[1], d.pitch[1], y);
                for (int x = 0; x < cw; x++) {
                    uv[2 * x] = u[x];
                    uv[2 * x + 1] = v[x];
                }
            }
        }
        break;
    case PixelFormat::YUY2:
    case PixelFormat::UYVY: {
        const bool uyvy = d.format == PixelFormat::UYVY;
        const int oy0 = uyvy ? 1 : 0, ou = uyvy ? 0 : 1, oy1 = uyvy ? 3 : 2, ov = uyvy ? 2 : 3;
        for (int y = 0; y < h; y++) {
            const uint8_t* yr = RowPtr(s.plane[0], s.pitch[0], y);
            const uint8_t* u = RowPtr(s.plane[1], s.pitch[1], y / 2);
            const uint8_t* v = RowPtr(s.plane[2], s.pitch[2], y / 2);
            uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
            for (int x = 0; x < cw; x++, out += 4) {
                out[oy0] = yr[2 * x];
                out[oy1] = (2 * x + 1 < w) ? yr[2 * x + 1] : yr[2 * x];
                out[ou] = u[x];
                out[ov] = v[x];
            }
        }
        break;
    }
    default: break;
    }
}

void I420ToRgbFamily(const ImageView& s, const ImageSpan& d, const Coeffs& c)
{
    const int w = s.width;
    for (int y = 0; y < s.height; y++) {
        const uint8_t* yr = RowPtr(s.plane[0], s.pitch[0], y);
        const uint8_t* u = RowPtr(s.plane[1], s.pitch[1], y / 2);
        const uint8_t* v = RowPtr(s.plane[2], s.pitch[2], y / 2);
        uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
        uint8_t r, g, b;
        switch (d.format) {
        case PixelFormat::BGRA:
            for (int x = 0; x < w; x++, out += 4) {
                YuvPixelToRgb(c, yr[x], u[x / 2], v[x / 2], &r, &g, &b);
                out[0] = b, out[1] = g, out[2] = r, out[3] = 255;
            }
            break;
        case PixelFormat::RGBA:
            for (int x = 0; x < w; x++, out += 4) {
                YuvPixelToRgb(c, yr[x], u[x / 2], v[x / 2], &r, &g, &b);
                out[0] = r, out[1] = g, out[2] = b, out[3] = 255;
            }
            break;
        case PixelFormat::BGR24:
            for (int x = 0; x < w; x++, out += 3) {
                YuvPixelToRgb(c, yr[x], u[x / 2], v[x / 2], &r, &g, &b);
                out[0] = b, out[1] = g, out[2] = r;
            }
            break;
        case PixelFormat::RGB24:
            for (int x = 0; x < w; x++, out += 3) {
                YuvPixelToRgb(c, yr[x], u[x / 2], v[x / 2], &r, &g, &b);
                out[0] = r, out[1] = g, out[2] = b;
            }
            break;
        case PixelFormat::Gray8:
            for (int x = 0; x < w; x++) {
                out[x] = Clamp255(((yr[x] - c.y_base) * c.ky + 32768) >> 16);
            }
            break;
        default: break;
        }
    }
}

// --- resampling --------------------------------------------------------------

constexpr int kWeightBits = 14;
constexpr int kWeightOne = 1 << kWeightBits;

// Tent-filter taps mapping the source window [win_start, win_start + win_size)
// onto dst_size outputs. The filter widens when downscaling so every source
// pixel contributes (area-style averaging), which avoids aliasing.
// Returns the number of taps per output; *identity is set when output i is
// exactly source pixel start[0] + i. pad_even rounds the tap count up to even
// (when the source is wide enough) for the SIMD kernels, which take taps in pairs.
int BuildTaps(int src_size, double win_start, double win_size, int dst_size, bool reverse, bool pad_even,
              std::vector<int32_t>& start, std::vector<int16_t>& weights, bool* identity)
{
    start.assign(static_cast<size_t>(dst_size), 0);
    const double rounded_start = std::floor(win_start + 0.5);
    *identity = !reverse && std::fabs(win_size - dst_size) < 1e-9 && std::fabs(win_start - rounded_start) < 1e-9 &&
                rounded_start >= 0 && rounded_start + dst_size <= src_size;
    if (*identity) {
        for (int i = 0; i < dst_size; i++) {
            start[static_cast<size_t>(i)] = static_cast<int32_t>(rounded_start) + i;
        }
        weights.assign(static_cast<size_t>(dst_size), static_cast<int16_t>(kWeightOne));
        return 1;
    }

    const double scale = win_size / dst_size;
    const double radius = std::max(1.0, scale);
    // Pixel j (centre j + 0.5) has a non-zero weight iff |j + 0.5 - center| < radius.
    auto range = [&](int i, int* lo, int* hi) {
        const double center = win_start + (i + 0.5) * scale;
        *lo = static_cast<int>(std::floor(center - radius - 0.5)) + 1;
        *hi = static_cast<int>(std::ceil(center + radius - 0.5)) - 1;
    };

    int n = 1;
    for (int i = 0; i < dst_size; i++) {
        int lo, hi;
        range(i, &lo, &hi);
        lo = std::clamp(lo, 0, src_size - 1);
        hi = std::clamp(hi, 0, src_size - 1);
        n = std::max(n, hi - lo + 1);
    }
    n = std::min(n, src_size);
    if (pad_even && (n & 1) && n + 1 <= src_size) {
        n++;
    }
    weights.assign(static_cast<size_t>(dst_size) * n, 0);

    std::vector<double> w(static_cast<size_t>(n));
    for (int i = 0; i < dst_size; i++) {
        const double center = win_start + (i + 0.5) * scale;
        int lo, hi;
        range(i, &lo, &hi);
        int first = std::clamp(lo, 0, src_size - 1);
        if (first + n > src_size) {
            first = src_size - n;
        }
        std::fill(w.begin(), w.end(), 0.0);
        double total = 0;
        for (int j = lo; j <= hi; j++) {
            const double wt = 1.0 - std::fabs(j + 0.5 - center) / radius;
            if (wt <= 0) {
                continue;
            }
            const int jc = std::clamp(j, 0, src_size - 1);
            w[static_cast<size_t>(jc - first)] += wt;
            total += wt;
        }
        const int out = reverse ? dst_size - 1 - i : i;
        int16_t* wo = &weights[static_cast<size_t>(out) * n];
        int sum = 0, best = 0;
        for (int k = 0; k < n; k++) {
            const int q = total > 0 ? static_cast<int>(std::lround(w[static_cast<size_t>(k)] / total * kWeightOne)) : 0;
            wo[k] = static_cast<int16_t>(q);
            sum += q;
            if (w[static_cast<size_t>(k)] > w[static_cast<size_t>(best)]) {
                best = k;
            }
        }
        wo[best] = static_cast<int16_t>(wo[best] + (kWeightOne - sum));
        start[static_cast<size_t>(out)] = first;
    }
    return n;
}

// One row through the horizontal taps. OutShift 14 yields 8-bit output; 7 keeps
// 7 extra bits (at most 32640, so the SIMD kernels can treat it as int16).
template <int C, typename Out, int OutShift>
void HorizontalRow(const uint8_t* in, const int32_t* start, const int16_t* weights, int n, int dst_w, Out* out)
{
    for (int x = 0; x < dst_w; x++) {
        const uint8_t* p = in + static_cast<ptrdiff_t>(start[x]) * C;
        const int16_t* w = weights + static_cast<ptrdiff_t>(x) * n;
        int32_t acc[C] = {};
        for (int k = 0; k < n; k++) {
            const int32_t wk = w[k];
            for (int c = 0; c < C; c++) {
                acc[c] += wk * p[k * C + c];
            }
        }
        for (int c = 0; c < C; c++) {
            out[x * C + c] = static_cast<Out>((acc[c] + (1 << (OutShift - 1))) >> OutShift);
        }
    }
}

template <typename In, int Shift>
void VerticalRow(const In* const* rows, const int16_t* weights, int n, int elems, int32_t* acc, uint8_t* out)
{
    std::fill(acc, acc + elems, 0);
    for (int k = 0; k < n; k++) {
        const int32_t wk = weights[k];
        if (wk == 0) {
            continue;
        }
        const In* r = rows[k];
        for (int i = 0; i < elems; i++) {
            acc[i] += wk * r[i];
        }
    }
    for (int i = 0; i < elems; i++) {
        out[i] = static_cast<uint8_t>((acc[i] + (1 << (Shift - 1))) >> Shift);
    }
}

template <int C>
void HorizontalRow8(const uint8_t* in, const int32_t* start, const int16_t* w, int n, int dw, uint8_t* out)
{
    HorizontalRow<C, uint8_t, kWeightBits>(in, start, w, n, dw, out);
}

template <int C>
void HorizontalRow16(const uint8_t* in, const int32_t* start, const int16_t* w, int n, int dw, uint16_t* out)
{
    HorizontalRow<C, uint16_t, kWeightBits - 7>(in, start, w, n, dw, out);
}

}  // namespace

// --- public helpers ----------------------------------------------------------

Placement ComputePlacement(int src_w, int src_h, int dst_w, int dst_h, ScaleMode mode, bool even_align)
{
    Placement p;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return p;
    }
    p.src_w = src_w;
    p.src_h = src_h;
    p.dst_w = dst_w;
    p.dst_h = dst_h;
    if (mode == ScaleMode::Fit) {
        const double scale = std::min(static_cast<double>(dst_w) / src_w, static_cast<double>(dst_h) / src_h);
        int w = std::clamp(static_cast<int>(std::lround(src_w * scale)), 1, dst_w);
        int h = std::clamp(static_cast<int>(std::lround(src_h * scale)), 1, dst_h);
        if (even_align) {
            if (w < dst_w) {
                w &= ~1;
            }
            if (h < dst_h) {
                h &= ~1;
            }
        }
        int x = (dst_w - w) / 2;
        int y = (dst_h - h) / 2;
        if (even_align) {
            x &= ~1;
            y &= ~1;
        }
        if (w <= 0 || h <= 0) {
            w = h = 0;
        }
        p.dst_x = x;
        p.dst_y = y;
        p.dst_w = w;
        p.dst_h = h;
    } else if (mode == ScaleMode::Fill) {
        const double src_aspect = static_cast<double>(src_w) / src_h;
        const double dst_aspect = static_cast<double>(dst_w) / dst_h;
        if (src_aspect > dst_aspect) {
            p.src_w = src_h * dst_aspect;
            p.src_x = (src_w - p.src_w) / 2;
        } else if (src_aspect < dst_aspect) {
            p.src_h = src_w / dst_aspect;
            p.src_y = (src_h - p.src_h) / 2;
        }
    }
    return p;
}

void RgbToYuv(Rgb rgb, ColorSpace colorspace, uint8_t* y, uint8_t* u, uint8_t* v)
{
    const Coeffs& c = GetCoeffs(colorspace);
    *y = LumaOf(c, rgb.r, rgb.g, rgb.b);
    *u = ChromaU(c, rgb.r, rgb.g, rgb.b, 0);
    *v = ChromaV(c, rgb.r, rgb.g, rgb.b, 0);
}

Rgb YuvToRgb(uint8_t y, uint8_t u, uint8_t v, ColorSpace colorspace)
{
    Rgb out;
    YuvPixelToRgb(GetCoeffs(colorspace), y, u, v, &out.r, &out.g, &out.b);
    return out;
}

bool FillImage(const ImageSpan& d, Rgb color, ColorSpace colorspace)
{
    if (!IsUsable(d)) {
        return false;
    }
    uint8_t yy, uu, vv;
    RgbToYuv(color, colorspace, &yy, &uu, &vv);
    const int w = d.width, h = d.height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    switch (d.format) {
    case PixelFormat::NV12:
    case PixelFormat::I420:
        for (int y = 0; y < h; y++) {
            memset(RowPtr(d.plane[0], d.pitch[0], y), yy, static_cast<size_t>(w));
        }
        for (int y = 0; y < ch; y++) {
            if (d.format == PixelFormat::NV12) {
                uint8_t* uv = RowPtr(d.plane[1], d.pitch[1], y);
                for (int x = 0; x < cw; x++) {
                    uv[2 * x] = uu;
                    uv[2 * x + 1] = vv;
                }
            } else {
                memset(RowPtr(d.plane[1], d.pitch[1], y), uu, static_cast<size_t>(cw));
                memset(RowPtr(d.plane[2], d.pitch[2], y), vv, static_cast<size_t>(cw));
            }
        }
        break;
    case PixelFormat::YUY2:
    case PixelFormat::UYVY: {
        const uint8_t mp[4] = {
            d.format == PixelFormat::YUY2 ? yy : uu,
            d.format == PixelFormat::YUY2 ? uu : yy,
            d.format == PixelFormat::YUY2 ? yy : vv,
            d.format == PixelFormat::YUY2 ? vv : yy,
        };
        for (int y = 0; y < h; y++) {
            uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
            for (int x = 0; x < cw; x++, out += 4) {
                memcpy(out, mp, 4);
            }
        }
        break;
    }
    default: {
        uint8_t px[4];
        const int bpp = RgbBytesPerPixel(d.format);
        switch (d.format) {
        case PixelFormat::BGRA: px[0] = color.b, px[1] = color.g, px[2] = color.r, px[3] = 255; break;
        case PixelFormat::RGBA: px[0] = color.r, px[1] = color.g, px[2] = color.b, px[3] = 255; break;
        case PixelFormat::BGR24: px[0] = color.b, px[1] = color.g, px[2] = color.r; break;
        case PixelFormat::RGB24: px[0] = color.r, px[1] = color.g, px[2] = color.b; break;
        default:  // Gray8
            px[0] = static_cast<uint8_t>((7471 * color.b + 38470 * color.g + 19595 * color.r + 32768) >> 16);
            break;
        }
        for (int y = 0; y < h; y++) {
            uint8_t* out = RowPtr(d.plane[0], d.pitch[0], y);
            for (int x = 0; x < w; x++, out += bpp) {
                memcpy(out, px, static_cast<size_t>(bpp));
            }
        }
        break;
    }
    }
    return true;
}

// --- Converter ---------------------------------------------------------------

ImageView Converter::Decode(const ImageView& src)
{
    if (src.format == PixelFormat::BGRA || src.format == PixelFormat::I420) {
        return src;
    }
    const int w = src.width, h = src.height;
    if (!IsYuv(src.format)) {
        decoded_.resize(static_cast<size_t>(w) * h * 4);
        RgbFamilyToBgra(src, decoded_.data(), w * 4);
        return MakeView(PixelFormat::BGRA, w, h, decoded_.data());
    }
    PlaneLayout layout;
    DefaultLayout(PixelFormat::I420, static_cast<uint32_t>(w), static_cast<uint32_t>(h), &layout);
    decoded_.resize(static_cast<size_t>(layout.total_bytes));
    ImageSpan i420 = MakeSpan(PixelFormat::I420, w, h, decoded_.data());
    YuvToI420(src, i420);
    ImageView view = i420.view();
    if (src.format == PixelFormat::NV12) {  // luma is already planar: use it in place
        view.plane[0] = src.plane[0];
        view.pitch[0] = src.pitch[0];
    }
    return view;
}

void Converter::ResamplePlane(const uint8_t* src, int src_pitch, int src_w, int src_h, int channels, double win_x,
                              double win_y, double win_w, double win_h, uint8_t* dst, int dst_pitch, int dst_w,
                              int dst_h, bool mirror, bool flip, bool force_alpha)
{
    const bool fast = simd::Available();
    bool h_identity = false, v_identity = false;
    const int hn = BuildTaps(src_w, win_x, win_w, dst_w, mirror, fast && channels == 4, htaps_start_, htaps_w_,
                             &h_identity);
    const int vn = BuildTaps(src_h, win_y, win_h, dst_h, flip, false, vtaps_start_, vtaps_w_, &v_identity);
    const int elems = dst_w * channels;
    const int32_t* hs = htaps_start_.data();
    const int16_t* hw = htaps_w_.data();

    // Weights rearranged for the SIMD kernels: int16 pairs for BGRA, 8 per output for planes.
    const bool simd_bgra = fast && channels == 4 && hn % 2 == 0;
    const bool simd_plane = fast && channels == 1 && hn <= 8;
    if (simd_bgra) {
        hpairs_.resize(static_cast<size_t>(dst_w) * (hn / 2));
        for (size_t i = 0; i < hpairs_.size(); i++) {
            hpairs_[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(hw[2 * i])) |
                                              (static_cast<uint32_t>(static_cast<uint16_t>(hw[2 * i + 1])) << 16));
        }
    } else if (simd_plane) {
        hw8_.assign(static_cast<size_t>(dst_w) * 8, 0);
        for (int x = 0; x < dst_w; x++) {
            for (int k = 0; k < hn; k++) {
                hw8_[static_cast<size_t>(x) * 8 + k] = hw[static_cast<size_t>(x) * hn + k];
            }
        }
    }

    auto hrow8 = [&](const uint8_t* in, uint8_t* out) {
        if (simd_bgra) {
            simd::HorizontalBgra8(in, hs, hpairs_.data(), hn / 2, dst_w, out);
        } else if (channels == 4) {
            HorizontalRow8<4>(in, hs, hw, hn, dst_w, out);
        } else {
            const int done = simd_plane ? simd::HorizontalPlane8(in, src_w, hs, hw8_.data(), dst_w, out) : 0;
            HorizontalRow8<1>(in, hs + done, hw + static_cast<ptrdiff_t>(done) * hn, hn, dst_w - done, out + done);
        }
    };
    auto hrow16 = [&](const uint8_t* in, uint16_t* out) {
        if (simd_bgra) {
            simd::HorizontalBgra16(in, hs, hpairs_.data(), hn / 2, dst_w, out);
        } else if (channels == 4) {
            HorizontalRow16<4>(in, hs, hw, hn, dst_w, out);
        } else {
            const int done = simd_plane ? simd::HorizontalPlane16(in, src_w, hs, hw8_.data(), dst_w, out) : 0;
            HorizontalRow16<1>(in, hs + done, hw + static_cast<ptrdiff_t>(done) * hn, hn, dst_w - done, out + done);
        }
    };

    if (h_identity && v_identity) {
        const ptrdiff_t x0 = static_cast<ptrdiff_t>(hs[0]) * channels;
        for (int y = 0; y < dst_h; y++) {
            memcpy(RowPtr(dst, dst_pitch, y), RowPtr(src, src_pitch, vtaps_start_[static_cast<size_t>(y)]) + x0,
                   static_cast<size_t>(elems));
        }
    } else if (v_identity) {
        for (int y = 0; y < dst_h; y++) {
            hrow8(RowPtr(src, src_pitch, vtaps_start_[static_cast<size_t>(y)]), RowPtr(dst, dst_pitch, y));
        }
    } else {
        acc_.resize(static_cast<size_t>(elems));
        if (h_identity) {
            const ptrdiff_t x0 = static_cast<ptrdiff_t>(hs[0]) * channels;
            std::vector<const uint8_t*> rows(static_cast<size_t>(vn));
            for (int y = 0; y < dst_h; y++) {
                const int first = vtaps_start_[static_cast<size_t>(y)];
                for (int k = 0; k < vn; k++) {
                    rows[static_cast<size_t>(k)] = RowPtr(src, src_pitch, first + k) + x0;
                }
                const int16_t* w = &vtaps_w_[static_cast<size_t>(y) * vn];
                if (fast) {
                    simd::VerticalRow8(rows.data(), w, vn, elems, RowPtr(dst, dst_pitch, y));
                } else {
                    VerticalRow<uint8_t, kWeightBits>(rows.data(), w, vn, elems, acc_.data(), RowPtr(dst, dst_pitch, y));
                }
            }
        } else {
            int rmin = src_h, rmax = 0;
            for (int y = 0; y < dst_h; y++) {
                rmin = std::min(rmin, vtaps_start_[static_cast<size_t>(y)]);
                rmax = std::max(rmax, vtaps_start_[static_cast<size_t>(y)] + vn - 1);
            }
            temp_.resize(static_cast<size_t>(rmax - rmin + 1) * elems);
            for (int r = rmin; r <= rmax; r++) {
                hrow16(RowPtr(src, src_pitch, r), &temp_[static_cast<size_t>(r - rmin) * elems]);
            }
            std::vector<const uint16_t*> rows(static_cast<size_t>(vn));
            for (int y = 0; y < dst_h; y++) {
                const int first = vtaps_start_[static_cast<size_t>(y)];
                for (int k = 0; k < vn; k++) {
                    rows[static_cast<size_t>(k)] = &temp_[static_cast<size_t>(first + k - rmin) * elems];
                }
                const int16_t* w = &vtaps_w_[static_cast<size_t>(y) * vn];
                if (fast) {
                    simd::VerticalRow16(rows.data(), w, vn, elems, RowPtr(dst, dst_pitch, y));
                } else {
                    VerticalRow<uint16_t, kWeightBits + 7>(rows.data(), w, vn, elems, acc_.data(),
                                                           RowPtr(dst, dst_pitch, y));
                }
            }
        }
    }

    if (force_alpha && channels == 4) {
        for (int y = 0; y < dst_h; y++) {
            uint8_t* row = RowPtr(dst, dst_pitch, y);
            for (int x = 3; x < elems; x += 4) {
                row[x] = 255;
            }
        }
    }
}

bool Converter::Resample(const ImageView& canon, const Placement& pl, const ImageSpan& out, bool mirror, bool flip)
{
    if (canon.format == PixelFormat::BGRA) {
        ResamplePlane(canon.plane[0], canon.pitch[0], canon.width, canon.height, 4, pl.src_x, pl.src_y, pl.src_w,
                      pl.src_h, out.plane[0], out.pitch[0], out.width, out.height, mirror, flip, true);
        return true;
    }
    ResamplePlane(canon.plane[0], canon.pitch[0], canon.width, canon.height, 1, pl.src_x, pl.src_y, pl.src_w,
                  pl.src_h, out.plane[0], out.pitch[0], out.width, out.height, mirror, flip, false);
    // A chroma sample covers two luma samples in each direction, so the chroma
    // window advances by the luma scale per output chroma sample. (Halving the
    // luma window would be wrong for odd sizes, where the last chroma sample
    // only half covers the picture.)
    const int scw = (canon.width + 1) / 2, sch = (canon.height + 1) / 2;
    const int dcw = (out.width + 1) / 2, dch = (out.height + 1) / 2;
    const double cwin_w = dcw * (pl.src_w / out.width);
    const double cwin_h = dch * (pl.src_h / out.height);
    for (int p = 1; p <= 2; p++) {
        ResamplePlane(canon.plane[p], canon.pitch[p], scw, sch, 1, pl.src_x / 2, pl.src_y / 2, cwin_w, cwin_h,
                      out.plane[p], out.pitch[p], dcw, dch, mirror, flip, false);
    }
    return true;
}

bool Converter::Convert(const ImageView& src, const ImageSpan& dst, const ConvertOptions& o)
{
    if (!IsUsable(src) || !IsUsable(dst)) {
        return false;
    }
    const Placement pl = ComputePlacement(src.width, src.height, dst.width, dst.height, o.scale, IsYuv(dst.format));
    const bool covers = pl.dst_x == 0 && pl.dst_y == 0 && pl.dst_w == dst.width && pl.dst_h == dst.height;
    if (!covers) {
        FillImage(dst, o.background, o.output_colorspace);
    }
    if (pl.dst_w <= 0 || pl.dst_h <= 0) {
        return true;
    }
    const bool whole_src = pl.src_x == 0 && pl.src_y == 0 && pl.src_w == src.width && pl.src_h == src.height;
    const bool unscaled = whole_src && src.width == pl.dst_w && src.height == pl.dst_h && !o.mirror && !o.flip;
    const bool same_cs = o.input_colorspace == o.output_colorspace;

    if (src.format == dst.format && covers && unscaled && (!IsYuv(src.format) || same_cs)) {
        CopyImage(src, dst);
        return true;
    }

    const ImageSpan target = covers ? dst : SubSpan(dst, pl.dst_x, pl.dst_y, pl.dst_w, pl.dst_h);
    const ImageView canon = Decode(src);

    // The destination already is the canonical format: scale straight into it.
    if (canon.format == dst.format && (canon.format == PixelFormat::BGRA || same_cs)) {
        Resample(canon, pl, target, o.mirror, o.flip);
        return true;
    }

    ImageView scaled = canon;
    if (!unscaled) {
        PlaneLayout layout;
        DefaultLayout(canon.format, static_cast<uint32_t>(pl.dst_w), static_cast<uint32_t>(pl.dst_h), &layout);
        scaled_.resize(static_cast<size_t>(layout.total_bytes));
        const ImageSpan s = MakeSpan(canon.format, pl.dst_w, pl.dst_h, scaled_.data());
        Resample(canon, pl, s, o.mirror, o.flip);
        scaled = s.view();
    }

    if (scaled.format == PixelFormat::BGRA) {
        if (target.format == PixelFormat::NV12 || target.format == PixelFormat::I420) {
            BgraToYuv420(scaled, target, GetCoeffs(o.output_colorspace));
        } else if (IsYuv(target.format)) {
            BgraToPacked422(scaled, target, GetCoeffs(o.output_colorspace));
        } else {
            BgraToRgbFamily(scaled, target);
        }
        return true;
    }

    // Canonical I420.
    if (!IsYuv(target.format)) {
        I420ToRgbFamily(scaled, target, GetCoeffs(o.input_colorspace));
    } else if (same_cs) {
        I420ToYuv(scaled, target);
    } else {
        // Different YUV matrix or range: re-encode through RGB.
        rgb_.resize(static_cast<size_t>(scaled.width) * scaled.height * 4);
        const ImageSpan tmp = MakeSpan(PixelFormat::BGRA, scaled.width, scaled.height, rgb_.data());
        I420ToRgbFamily(scaled, tmp, GetCoeffs(o.input_colorspace));
        if (target.format == PixelFormat::NV12 || target.format == PixelFormat::I420) {
            BgraToYuv420(tmp.view(), target, GetCoeffs(o.output_colorspace));
        } else {
            BgraToPacked422(tmp.view(), target, GetCoeffs(o.output_colorspace));
        }
    }
    return true;
}

}  // namespace vcam
