// Unit tests for the portable core. Plain C++, no framework; runs on Linux and Windows.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "vcam/convert.h"
#include "vcam/draw.h"
#include "vcam/format.h"
#include "vcam/modes.h"
#include "vcam/pacer.h"
#include "vcam/ring.h"
#include "../core/src/simd.h"

using namespace vcam;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "  FAILED %s:%d: %s: ", __FILE__, __LINE__, #cond);  \
            fprintf(stderr, __VA_ARGS__);                                        \
            fprintf(stderr, "\n");                                               \
        }                                                                        \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                     \
    static void name();                                \
    static Register register_##name(#name, name);      \
    static void name()

// An owned image in any format.
struct Buffer {
    std::vector<uint8_t> bytes;
    ImageSpan span;

    Buffer(PixelFormat f, int w, int h)
    {
        PlaneLayout layout;
        DefaultLayout(f, static_cast<uint32_t>(w), static_cast<uint32_t>(h), &layout);
        bytes.assign(static_cast<size_t>(layout.total_bytes), 0xCD);
        span = MakeSpan(f, w, h, bytes.data());
    }
    ImageView view() const { return span.view(); }
};

// BGRA test image that is constant over 2x2 blocks, so 4:2:0 subsampling is lossless.
Buffer BlockyBgra(int w, int h, uint32_t seed)
{
    Buffer b(PixelFormat::BGRA, w, h);
    std::mt19937 rng(seed);
    std::vector<uint8_t> colors(static_cast<size_t>((w + 1) / 2 * ((h + 1) / 2) * 3));
    for (auto& c : colors) {
        c = static_cast<uint8_t>(rng());
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* c = &colors[static_cast<size_t>(((y / 2) * ((w + 1) / 2) + x / 2) * 3)];
            uint8_t* px = b.span.plane[0] + y * b.span.pitch[0] + x * 4;
            px[0] = c[0], px[1] = c[1], px[2] = c[2], px[3] = 255;
        }
    }
    return b;
}

const uint8_t* Px(const ImageView& v, int x, int y)
{
    return v.plane[0] + static_cast<ptrdiff_t>(v.pitch[0]) * y + x * 4;
}

int MaxDiffBgra(const ImageView& a, const ImageView& b)
{
    int worst = 0;
    for (int y = 0; y < a.height; y++) {
        for (int x = 0; x < a.width; x++) {
            for (int c = 0; c < 3; c++) {
                worst = std::max(worst, std::abs(Px(a, x, y)[c] - Px(b, x, y)[c]));
            }
        }
    }
    return worst;
}

const PixelFormat kAllFormats[] = {
    PixelFormat::BGRA, PixelFormat::RGBA, PixelFormat::BGR24, PixelFormat::RGB24, PixelFormat::Gray8,
    PixelFormat::NV12, PixelFormat::I420, PixelFormat::YUY2,  PixelFormat::UYVY,
};

// ---------------------------------------------------------------------------

TEST(FormatGeometry)
{
    PlaneLayout l;
    CHECK(DefaultLayout(PixelFormat::NV12, 1920, 1080, &l));
    CHECK(l.count == 2 && l.total_bytes == 1920u * 1080 * 3 / 2 && l.offset[1] == 1920u * 1080);
    CHECK(DefaultLayout(PixelFormat::I420, 5, 3, &l));
    CHECK(l.count == 3 && l.pitch[1] == 3 && l.total_bytes == 15u + 2 * 3 * 2);
    CHECK(DefaultLayout(PixelFormat::YUY2, 5, 2, &l));
    CHECK(l.pitch[0] == 12 && l.total_bytes == 24);
    CHECK(DefaultLayout(PixelFormat::BGRA, 3, 3, &l, 64));
    CHECK(l.pitch[0] == 64);
    CHECK(!DefaultLayout(PixelFormat::Unknown, 4, 4, &l));
    CHECK(!DefaultLayout(PixelFormat::BGRA, 0, 4, &l));
    CHECK(!DefaultLayout(PixelFormat::BGRA, kMaxDimension + 1, 4, &l));
    CHECK(ParseFormat("nv12") == PixelFormat::NV12);
    CHECK(ParseFormat("RGB") == PixelFormat::RGB24);
    CHECK(ParseFormat("bogus") == PixelFormat::Unknown);
    CHECK(ParseFormat("") == PixelFormat::Unknown);
    for (PixelFormat f : kAllFormats) {
        CHECK(ParseFormat(FormatName(f)) == f);
    }
}

TEST(ColorMath)
{
    uint8_t y, u, v;
    RgbToYuv(Rgb{255, 255, 255}, ColorSpace::Bt601Limited, &y, &u, &v);
    CHECK(y == 235 && u == 128 && v == 128);
    RgbToYuv(Rgb{0, 0, 0}, ColorSpace::Bt601Limited, &y, &u, &v);
    CHECK(y == 16 && u == 128 && v == 128);
    RgbToYuv(Rgb{255, 255, 255}, ColorSpace::Bt709Full, &y, &u, &v);
    CHECK(y == 255 && u == 128 && v == 128);
    RgbToYuv(Rgb{128, 128, 128}, ColorSpace::Bt709Limited, &y, &u, &v);
    CHECK(u == 128 && v == 128);
    // Pure red in BT.601 limited range is Y=81, U=90, V=240.
    RgbToYuv(Rgb{255, 0, 0}, ColorSpace::Bt601Limited, &y, &u, &v);
    CHECK_MSG(std::abs(y - 81) <= 1 && std::abs(u - 90) <= 1 && std::abs(v - 240) <= 1, "%d %d %d", y, u, v);
    // Pure blue in BT.709 limited range is Y=32, U=240, V=118.
    RgbToYuv(Rgb{0, 0, 255}, ColorSpace::Bt709Limited, &y, &u, &v);
    CHECK_MSG(std::abs(y - 32) <= 1 && std::abs(u - 240) <= 1 && std::abs(v - 118) <= 1, "%d %d %d", y, u, v);

    std::mt19937 rng(1);
    for (int cs = 0; cs < 4; cs++) {
        int worst = 0;
        for (int i = 0; i < 20000; i++) {
            const Rgb in{static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng())};
            RgbToYuv(in, static_cast<ColorSpace>(cs), &y, &u, &v);
            const Rgb out = YuvToRgb(y, u, v, static_cast<ColorSpace>(cs));
            worst = std::max({worst, std::abs(in.r - out.r), std::abs(in.g - out.g), std::abs(in.b - out.b)});
        }
        // Limited range quantises to fewer levels, so allow a few codes.
        CHECK_MSG(worst <= 3, "colorspace %d round trip error %d", cs, worst);
    }
}

TEST(ConvertRoundTripAllPairs)
{
    const int sizes[][2] = {{64, 48}, {33, 17}, {2, 2}, {1, 1}, {7, 1}};
    Converter conv;
    ConvertOptions opt;
    for (auto& sz : sizes) {
        const int w = sz[0], h = sz[1];
        Buffer src = BlockyBgra(w, h, static_cast<uint32_t>(w * 31 + h));
        for (PixelFormat a : kAllFormats) {
            for (PixelFormat b : kAllFormats) {
                Buffer ia(a, w, h), ib(b, w, h), back(PixelFormat::BGRA, w, h);
                CHECK(conv.Convert(src.view(), ia.span, opt));
                CHECK(conv.Convert(ia.view(), ib.span, opt));
                CHECK(conv.Convert(ib.view(), back.span, opt));
                int tolerance = 0;
                const bool gray = a == PixelFormat::Gray8 || b == PixelFormat::Gray8;
                if (gray) {
                    // Gray loses colour: compare against the grey version instead.
                    Buffer g(PixelFormat::Gray8, w, h), gb(PixelFormat::BGRA, w, h);
                    conv.Convert(src.view(), g.span, opt);
                    conv.Convert(g.view(), gb.span, opt);
                    const bool yuv_path = IsYuv(a) || IsYuv(b);
                    tolerance = yuv_path ? 4 : 0;
                    const int d = MaxDiffBgra(gb.view(), back.view());
                    CHECK_MSG(d <= tolerance, "%s->%s %dx%d gray diff %d", FormatName(a), FormatName(b), w, h, d);
                    continue;
                }
                if (IsYuv(a) || IsYuv(b)) {
                    tolerance = 4;  // limited-range quantisation
                }
                const int d = MaxDiffBgra(src.view(), back.view());
                CHECK_MSG(d <= tolerance, "%s->%s %dx%d diff %d", FormatName(a), FormatName(b), w, h, d);
            }
        }
    }
}

TEST(ConvertSetsOpaqueAlpha)
{
    Buffer src(PixelFormat::BGRA, 8, 8);
    std::fill(src.bytes.begin(), src.bytes.end(), 0);  // alpha 0, as BGRX captures often have
    Buffer dst(PixelFormat::BGRA, 8, 8), scaled(PixelFormat::BGRA, 5, 3);
    Converter conv;
    conv.Convert(src.view(), dst.span, ConvertOptions{});
    conv.Convert(src.view(), scaled.span, ConvertOptions{});
    bool opaque = true;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) opaque &= Px(dst.view(), x, y)[3] == 255;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 5; x++) opaque &= Px(scaled.view(), x, y)[3] == 255;
    CHECK(opaque);
}

TEST(ScalingKeepsFlatColorExact)
{
    Converter conv;
    ConvertOptions opt;
    opt.scale = ScaleMode::Stretch;
    Buffer src(PixelFormat::BGRA, 37, 23);
    FillImage(src.span, Rgb{10, 200, 77}, ColorSpace::Bt601Limited);
    const int targets[][2] = {{1, 1}, {640, 360}, {5, 300}, {36, 22}, {38, 24}, {200, 3}};
    for (auto& t : targets) {
        Buffer dst(PixelFormat::BGRA, t[0], t[1]);
        CHECK(conv.Convert(src.view(), dst.span, opt));
        bool exact = true;
        for (int y = 0; y < t[1]; y++)
            for (int x = 0; x < t[0]; x++) {
                const uint8_t* p = Px(dst.view(), x, y);
                exact &= p[0] == 77 && p[1] == 200 && p[2] == 10 && p[3] == 255;
            }
        CHECK_MSG(exact, "%dx%d", t[0], t[1]);
    }
}

TEST(DownscaleAveragesInsteadOfAliasing)
{
    // A one-pixel checkerboard shrunk 4x must become uniform mid grey, not a moire pattern.
    Buffer src(PixelFormat::Gray8, 256, 256);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) src.span.plane[0][y * src.span.pitch[0] + x] = ((x + y) & 1) ? 255 : 0;
    Buffer dst(PixelFormat::BGRA, 64, 64);
    Converter conv;
    CHECK(conv.Convert(src.view(), dst.span, ConvertOptions{}));
    int lo = 255, hi = 0;
    for (int y = 1; y < 63; y++)
        for (int x = 1; x < 63; x++) {
            lo = std::min<int>(lo, Px(dst.view(), x, y)[1]);
            hi = std::max<int>(hi, Px(dst.view(), x, y)[1]);
        }
    CHECK_MSG(lo >= 120 && hi <= 135, "range %d..%d", lo, hi);
}

TEST(UpscaleIsSmoothAndMonotonic)
{
    Buffer src(PixelFormat::Gray8, 16, 1);
    for (int x = 0; x < 16; x++) src.span.plane[0][x] = static_cast<uint8_t>(x * 17);
    Buffer dst(PixelFormat::BGRA, 160, 1);
    ConvertOptions opt;
    opt.scale = ScaleMode::Stretch;
    Converter conv;
    CHECK(conv.Convert(src.view(), dst.span, opt));
    bool monotonic = true;
    for (int x = 1; x < 160; x++) monotonic &= Px(dst.view(), x, 0)[0] >= Px(dst.view(), x - 1, 0)[0];
    CHECK(monotonic);
    CHECK(Px(dst.view(), 0, 0)[0] == 0 && Px(dst.view(), 159, 0)[0] == 255);
}

TEST(PlacementFitFillStretch)
{
    // 4:3 into 16:9 -> pillarbox.
    Placement p = ComputePlacement(640, 480, 1280, 720, ScaleMode::Fit, true);
    CHECK(p.dst_y == 0 && p.dst_h == 720 && p.dst_w == 960 && p.dst_x == 160);
    // 16:9 into 4:3 -> letterbox.
    p = ComputePlacement(1920, 1080, 640, 480, ScaleMode::Fit, true);
    CHECK(p.dst_x == 0 && p.dst_w == 640 && p.dst_h == 360 && p.dst_y == 60);
    // Odd sizes stay on even coordinates.
    p = ComputePlacement(101, 77, 640, 480, ScaleMode::Fit, true);
    CHECK(p.dst_x % 2 == 0 && p.dst_y % 2 == 0 && p.dst_w % 2 == 0 && p.dst_h % 2 == 0);
    // Fill crops the source.
    p = ComputePlacement(1920, 1080, 640, 480, ScaleMode::Fill, true);
    CHECK(p.dst_w == 640 && p.dst_h == 480 && std::fabs(p.src_w - 1440) < 1e-6 && std::fabs(p.src_x - 240) < 1e-6);
    p = ComputePlacement(1920, 1080, 640, 480, ScaleMode::Stretch, true);
    CHECK(p.src_w == 1920 && p.src_h == 1080 && p.dst_w == 640 && p.dst_h == 480);
}

TEST(LetterboxPaddingIsBackground)
{
    Buffer src(PixelFormat::BGRA, 100, 100);
    FillImage(src.span, Rgb{255, 255, 255}, ColorSpace::Bt601Limited);
    Buffer dst(PixelFormat::NV12, 200, 100);
    ConvertOptions opt;
    opt.background = Rgb{0, 0, 0};
    Converter conv;
    CHECK(conv.Convert(src.view(), dst.span, opt));
    const uint8_t* y = dst.span.plane[0];
    CHECK(y[10] == 16 && y[100] == 235 && y[190] == 16);
    CHECK(y[49] == 16 && y[50] == 235 && y[149] == 235 && y[150] == 16);
}

TEST(MirrorAndFlip)
{
    Buffer src(PixelFormat::BGRA, 4, 2);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t* p = src.span.plane[0] + y * src.span.pitch[0] + x * 4;
            p[0] = static_cast<uint8_t>(x * 10 + y * 100), p[1] = p[2] = 0, p[3] = 255;
        }
    Converter conv;
    ConvertOptions opt;
    opt.mirror = true;
    Buffer m(PixelFormat::BGRA, 4, 2);
    CHECK(conv.Convert(src.view(), m.span, opt));
    CHECK(Px(m.view(), 0, 0)[0] == 30 && Px(m.view(), 3, 0)[0] == 0 && Px(m.view(), 0, 1)[0] == 130);
    opt.mirror = false;
    opt.flip = true;
    Buffer f(PixelFormat::BGRA, 4, 2);
    CHECK(conv.Convert(src.view(), f.span, opt));
    CHECK(Px(f.view(), 0, 0)[0] == 100 && Px(f.view(), 3, 1)[0] == 30);
}

TEST(NegativePitchSource)
{
    // A bottom-up BGRA image (negative pitch, pointer at the top row).
    Buffer storage(PixelFormat::BGRA, 3, 3);
    for (int row = 0; row < 3; row++) {
        // Memory row 0 holds the bottom image row.
        std::fill(storage.bytes.begin() + row * 12, storage.bytes.begin() + row * 12 + 12,
                  static_cast<uint8_t>(row * 50));
    }
    ImageView v;
    v.format = PixelFormat::BGRA;
    v.width = 3;
    v.height = 3;
    v.plane[0] = storage.bytes.data() + 2 * 12;
    v.pitch[0] = -12;
    Buffer dst(PixelFormat::BGRA, 3, 3);
    Converter conv;
    CHECK(conv.Convert(v, dst.span, ConvertOptions{}));
    CHECK(Px(dst.view(), 0, 0)[0] == 100 && Px(dst.view(), 0, 2)[0] == 0);
}

TEST(ColorspaceConversionBetweenYuvFormats)
{
    Buffer src = BlockyBgra(32, 32, 5);
    Buffer a(PixelFormat::NV12, 32, 32), b(PixelFormat::NV12, 32, 32), back(PixelFormat::BGRA, 32, 32);
    Converter conv;
    ConvertOptions to709;
    to709.output_colorspace = ColorSpace::Bt709Limited;
    CHECK(conv.Convert(src.view(), a.span, to709));
    ConvertOptions recode;
    recode.input_colorspace = ColorSpace::Bt709Limited;
    recode.output_colorspace = ColorSpace::Bt601Full;
    CHECK(conv.Convert(a.view(), b.span, recode));
    ConvertOptions from601full;
    from601full.input_colorspace = ColorSpace::Bt601Full;
    CHECK(conv.Convert(b.view(), back.span, from601full));
    CHECK_MSG(MaxDiffBgra(src.view(), back.view()) <= 6, "%d", MaxDiffBgra(src.view(), back.view()));
}

TEST(InvalidViewsAreRejected)
{
    Converter conv;
    Buffer ok(PixelFormat::BGRA, 4, 4);
    ImageView bad = ok.view();
    bad.pitch[0] = 8;  // shorter than a row
    CHECK(!conv.Convert(bad, ok.span, ConvertOptions{}));
    ImageView none;
    CHECK(!conv.Convert(none, ok.span, ConvertOptions{}));
}

TEST(SimdMatchesScalarExactly)
{
    if (!simd::Available()) {
        printf("  (no SIMD on this machine, skipped)\n");
        return;
    }
    std::mt19937 rng(11);
    Converter fast, slow;
    for (int iter = 0; iter < 400; iter++) {
        const PixelFormat in = kAllFormats[rng() % 9];
        const PixelFormat out = kAllFormats[rng() % 9];
        const int sw = 1 + static_cast<int>(rng() % 97), sh = 1 + static_cast<int>(rng() % 61);
        const int dw = 1 + static_cast<int>(rng() % 131), dh = 1 + static_cast<int>(rng() % 71);
        Buffer src(in, sw, sh);
        for (auto& b : src.bytes) b = static_cast<uint8_t>(rng());
        ConvertOptions opt;
        opt.scale = static_cast<ScaleMode>(rng() % 3);
        opt.mirror = rng() % 2;
        opt.flip = rng() % 4 == 0;
        opt.input_colorspace = static_cast<ColorSpace>(rng() % 4);
        opt.output_colorspace = static_cast<ColorSpace>(rng() % 4);
        Buffer a(out, dw, dh), b(out, dw, dh);
        simd::SetEnabled(true);
        CHECK(fast.Convert(src.view(), a.span, opt));
        simd::SetEnabled(false);
        CHECK(slow.Convert(src.view(), b.span, opt));
        simd::SetEnabled(true);
        CHECK_MSG(a.bytes == b.bytes, "%s %dx%d -> %s %dx%d mode %u mirror %d", FormatName(in), sw, sh,
                  FormatName(out), dw, dh, static_cast<unsigned>(opt.scale), opt.mirror);
    }
    // Larger frames exercise the vector loops' main bodies.
    for (int iter = 0; iter < 6; iter++) {
        Buffer src = BlockyBgra(640 + iter * 37, 360 + iter * 11, static_cast<uint32_t>(iter));
        for (PixelFormat out : {PixelFormat::NV12, PixelFormat::YUY2, PixelFormat::UYVY, PixelFormat::I420,
                                PixelFormat::BGRA}) {
            Buffer a(out, 1280 - iter * 64, 720 - iter * 30), b(out, 1280 - iter * 64, 720 - iter * 30);
            simd::SetEnabled(true);
            fast.Convert(src.view(), a.span, ConvertOptions{});
            simd::SetEnabled(false);
            slow.Convert(src.view(), b.span, ConvertOptions{});
            simd::SetEnabled(true);
            CHECK_MSG(a.bytes == b.bytes, "large %s", FormatName(out));
        }
    }
}

// --- ring --------------------------------------------------------------------

struct Mapping {
    std::vector<uint64_t> words;  // 8-byte aligned storage
    uint64_t bytes;
    explicit Mapping(uint64_t capacity) : bytes(ShmBytesFor(capacity))
    {
        words.assign(static_cast<size_t>((bytes + 63) / 8 + 8), 0);
    }
    void* base()
    {
        // Align to 64 like a real mapping (page aligned).
        auto p = reinterpret_cast<uintptr_t>(words.data());
        return reinterpret_cast<void*>((p + 63) & ~uintptr_t(63));
    }
};

TEST(RingWriteRead)
{
    const uint64_t cap = SlotCapacityFor(64, 64);
    Mapping m(cap);
    Ring writer, reader;
    CHECK(!reader.Attach(m.base(), m.bytes));  // not initialised yet
    CHECK(InitializeShm(m.base(), m.bytes, cap));
    CHECK(writer.Attach(m.base(), m.bytes));
    CHECK(reader.Attach(m.base(), m.bytes));
    CHECK(reader.ReadLatest([](const ImageView&, const shm::SlotHeader&) {}) == Ring::ReadStatus::NoFrame);

    for (int i = 1; i <= 10; i++) {
        shm::SlotHeader info{};
        CHECK(LayoutSlot(PixelFormat::NV12, 32, 16, &info));
        info.timestamp = i * 1000;
        Ring::WriteTarget t = writer.BeginWrite();
        CHECK(t.data && t.frame == static_cast<uint64_t>(i) && info.data_bytes <= t.capacity);
        memset(t.data, i, info.data_bytes);
        writer.CommitWrite(t, info);

        uint64_t frame = 0;
        bool seen = false;
        const auto status = reader.ReadLatest(
            [&](const ImageView& v, const shm::SlotHeader& h) {
                seen = v.format == PixelFormat::NV12 && v.width == 32 && v.height == 16 && v.plane[1][0] == i &&
                       h.timestamp == i * 1000;
            },
            &frame);
        CHECK(status == Ring::ReadStatus::Ok && seen && frame == static_cast<uint64_t>(i));
    }
    CHECK(m.base() && reinterpret_cast<shm::Header*>(m.base())->frames_written == 10);
}

TEST(RingRejectsCorruptHeaders)
{
    const uint64_t cap = SlotCapacityFor(64, 64);
    Mapping m(cap);
    CHECK(InitializeShm(m.base(), m.bytes, cap));
    Ring ring;
    CHECK(ring.Attach(m.base(), m.bytes));
    CHECK(!ring.Attach(m.base(), m.bytes - 4096));  // mapping smaller than advertised
    auto* h = static_cast<shm::Header*>(m.base());
    const uint64_t saved = h->slot_capacity;
    h->slot_capacity = ~0ull;
    CHECK(!ring.Attach(m.base(), m.bytes));
    h->slot_capacity = saved;
    CHECK(ring.Attach(m.base(), m.bytes));

    // Random slot headers must never validate into a view that leaves the slot.
    std::mt19937_64 rng(7);
    std::vector<uint8_t> slot(static_cast<size_t>(cap));
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        shm::SlotHeader s{};
        s.format = static_cast<uint32_t>(rng() % 12);
        s.width = static_cast<uint32_t>(rng() % 3 == 0 ? rng() : rng() % 80);
        s.height = static_cast<uint32_t>(rng() % 3 == 0 ? rng() : rng() % 80);
        s.colorspace = static_cast<uint32_t>(rng() % 5);
        for (int p = 0; p < 3; p++) {
            s.offset[p] = static_cast<uint32_t>(rng() % 2 ? rng() : rng() % cap);
            s.pitch[p] = static_cast<uint32_t>(rng() % 2 ? rng() : rng() % 400);
        }
        s.data_bytes = static_cast<uint32_t>(rng() % 2 ? rng() : rng() % (cap + 100));
        ImageView v;
        if (ValidateSlot(s, cap, slot.data(), &v)) {
            accepted++;
            for (int p = 0; p < PlaneCount(v.format); p++) {
                uint32_t rb, rows;
                PlaneGeometry(v.format, static_cast<uint32_t>(v.width), static_cast<uint32_t>(v.height), p, &rb, &rows);
                const uint8_t* last = v.plane[p] + static_cast<ptrdiff_t>(v.pitch[p]) * (rows - 1) + rb;
                CHECK(v.plane[p] >= slot.data() && last <= slot.data() + cap);
            }
        }
    }
    CHECK(accepted > 0);
}

TEST(RingConcurrentReaderNeverSeesTornFrames)
{
    const uint64_t cap = SlotCapacityFor(256, 256);
    Mapping m(cap);
    CHECK(InitializeShm(m.base(), m.bytes, cap));
    Ring writer, reader;
    CHECK(writer.Attach(m.base(), m.bytes) && reader.Attach(m.base(), m.bytes));
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0}, ok{0};

    std::thread w([&] {
        shm::SlotHeader info{};
        LayoutSlot(PixelFormat::BGRA, 256, 256, &info);
        while (!stop.load()) {
            Ring::WriteTarget t = writer.BeginWrite();
            const uint8_t value = static_cast<uint8_t>(t.frame);
            for (uint32_t i = 0; i < info.data_bytes; i += 64) {
                memset(t.data + i, value, std::min<uint32_t>(64, info.data_bytes - i));
            }
            writer.CommitWrite(t, info);
        }
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < until) {
        bool uniform = true;
        uint64_t frame = 0;
        const auto status = reader.ReadLatest(
            [&](const ImageView& v, const shm::SlotHeader& h) {
                uniform = true;  // fn runs again when a torn read is retried
                const uint8_t first = v.plane[0][0];
                for (uint32_t i = 0; i < h.data_bytes; i += 61) {
                    if (v.plane[0][i] != first) {
                        uniform = false;
                        break;
                    }
                }
                uniform = uniform && v.plane[0][h.data_bytes - 1] == first;
            },
            &frame);
        if (status == Ring::ReadStatus::Ok) {
            ok++;
            if (!uniform) {
                torn++;
            }
        }
    }
    stop = true;
    w.join();
    CHECK_MSG(torn.load() == 0, "%d torn of %d", torn.load(), ok.load());
    CHECK(ok.load() > 0);
}

TEST(RingRequestSeqlock)
{
    const uint64_t cap = SlotCapacityFor(16, 16);
    Mapping m(cap);
    CHECK(InitializeShm(m.base(), m.bytes, cap));
    Ring a, b;
    CHECK(a.Attach(m.base(), m.bytes) && b.Attach(m.base(), m.bytes));
    shm::FrameRequest r;
    r.format = static_cast<uint32_t>(PixelFormat::YUY2);
    r.width = 1280;
    r.height = 720;
    r.fps_num = 30;
    r.fps_den = 1;
    a.WriteRequest(r);
    shm::FrameRequest got;
    CHECK(b.ReadRequest(&got));
    CHECK(got.format == r.format && got.width == 1280 && got.height == 720 && got.fps_num == 30);
    // A writer that died mid-update leaves an odd sequence; the next write recovers.
    auto* h = static_cast<shm::Header*>(m.base());
    h->request_seq |= 1;
    r.width = 640;
    a.WriteRequest(r);
    CHECK(b.ReadRequest(&got) && got.width == 640);
}

TEST(Liveness)
{
    shm::Header h{};
    CHECK(!ProducerAlive(&h, 100));
    h.producer_pid = 42;
    h.producer_heartbeat = 1000;
    CHECK(ProducerAlive(&h, 1000 + shm::kHeartbeatTimeout - 1));
    CHECK(!ProducerAlive(&h, 1000 + shm::kHeartbeatTimeout));
    h.consumer_flags = shm::kConsumerStreaming;
    h.consumer_heartbeat = 50;
    CHECK(ConsumerStreaming(&h, 60) && !ConsumerStreaming(&h, 50 + shm::kHeartbeatTimeout));
}

// --- pacer -------------------------------------------------------------------

struct PaceStats {
    int delivered = 0;
    int fresh = 0;
    int64_t max_gap = 0;
    int64_t max_latency = 0;
};

// Simulates a 1 ms-resolution scheduler: producer frames every producer_interval
// (0 = no producer), camera at camera_interval, for `seconds`.
PaceStats Simulate(int64_t camera_interval, int64_t producer_interval, double seconds, int64_t jitter = 0)
{
    FramePacer pacer;
    PaceStats s;
    const int64_t ms = 10000;
    int64_t now = 0;
    pacer.Start(camera_interval, now);
    uint64_t latest = 0;
    int64_t next_frame = producer_interval;
    int64_t latest_time = 0;
    int64_t last = -1;
    std::mt19937 rng(3);
    const int64_t end = static_cast<int64_t>(seconds * 1e7);
    for (; now < end; now += ms) {
        if (producer_interval && now >= next_frame) {
            latest++;
            latest_time = now;
            next_frame += producer_interval + (jitter ? static_cast<int64_t>(rng() % (2 * jitter)) - jitter : 0);
        }
        int64_t wake = 0;
        if (pacer.Poll(now, latest, producer_interval != 0, &wake)) {
            const bool is_fresh = latest != 0 && latest != pacer.last_frame();
            if (is_fresh) {
                s.fresh++;
                s.max_latency = std::max(s.max_latency, now - latest_time);
            }
            pacer.Delivered(now, latest);
            if (last >= 0) {
                s.max_gap = std::max(s.max_gap, now - last);
            }
            last = now;
            s.delivered++;
        }
    }
    return s;
}

TEST(PacerHoldsNominalRateWithoutProducer)
{
    const PaceStats s = Simulate(333333, 0, 10.0);
    CHECK_MSG(s.delivered >= 297 && s.delivered <= 302, "%d", s.delivered);
    CHECK(s.max_gap <= 333333 + 10000);
}

TEST(PacerFollowsMatchingProducerWithLowLatency)
{
    const PaceStats s = Simulate(333333, 333333, 10.0, 40000);
    CHECK_MSG(s.delivered >= 290 && s.delivered <= 305, "%d", s.delivered);
    CHECK_MSG(s.fresh >= 285, "fresh %d", s.fresh);
    CHECK_MSG(s.max_latency <= 333333, "latency %lld", static_cast<long long>(s.max_latency));
}

TEST(PacerCapsFasterProducer)
{
    const PaceStats s = Simulate(333333, 166667, 10.0);
    CHECK_MSG(s.delivered >= 290 && s.delivered <= 305, "%d", s.delivered);
}

TEST(PacerFillsInForSlowerProducer)
{
    const PaceStats s = Simulate(333333, 666667, 10.0);
    CHECK_MSG(s.fresh >= 145, "fresh %d", s.fresh);
    CHECK_MSG(s.delivered >= 250, "%d", s.delivered);
    // A fresh frame may be sent with 0.8 intervals of credit; the following
    // repeat then needs 1.7 intervals. That is the worst case by design.
    CHECK_MSG(s.max_gap <= 333333 * 17 / 10 + 10000, "gap %lld", static_cast<long long>(s.max_gap));
}

TEST(PacerRateIsBoundedUnderHeavyJitter)
{
    for (int64_t producer : {166667, 333333, 400000}) {
        const PaceStats s = Simulate(333333, producer, 20.0, 120000);
        CHECK_MSG(s.delivered <= 602, "producer %lld delivered %d", static_cast<long long>(producer), s.delivered);
        CHECK_MSG(s.max_gap <= 333333 * 17 / 10 + 10000, "producer %lld gap %lld", static_cast<long long>(producer),
                  static_cast<long long>(s.max_gap));
    }
}

// --- modes / drawing -----------------------------------------------------------

TEST(StandardModeList)
{
    auto modes = StandardModes(1920, 1080, true);
    CHECK(!modes.empty() && modes[0].width == 1920 && modes[0].height == 1080 && modes[0].fps == 30);
    for (const auto& m : modes) CHECK(m.width <= 1920 && m.height <= 1080 && (m.fps == 30 || m.fps == 60));
    modes = StandardModes(3840, 2160, false);
    CHECK(modes[0].width == 1920 && modes.size() == 12);
    modes = StandardModes(100, 100, true);
    CHECK(modes.size() == 2 && modes[0].width == 320);
    uint32_t w = 0, h = 0;
    CHECK(ParseResolution("4K", &w, &h) && w == 3840 && h == 2160);
    CHECK(ParseResolution("1280x720", &w, &h) && w == 1280 && h == 720);
    CHECK(!ParseResolution("1280x", &w, &h) && !ParseResolution("x720", &w, &h) && !ParseResolution("big", &w, &h));
    CHECK(!ParseResolution("99999x10", &w, &h));
}

TEST(DrawingNeverWritesOutOfBounds)
{
    const int sizes[][2] = {{1920, 1080}, {640, 480}, {33, 17}, {16, 16}, {1, 1}, {3, 200}};
    for (auto& sz : sizes) {
        const int w = sz[0], h = sz[1];
        // Guard bytes around the image detect overruns.
        std::vector<uint8_t> mem(static_cast<size_t>(w) * h * 4 + 128, 0xAB);
        ImageSpan img = MakeSpan(PixelFormat::BGRA, w, h, mem.data() + 64);
        const char* lines[] = {"VCAM TEST PATTERN", "1920x1080 NV12 30 fps", "12:34:56.789"};
        RenderTestPattern(img, 7, 1.25, lines, 3);
        bool guard = std::all_of(mem.begin(), mem.begin() + 64, [](uint8_t b) { return b == 0xAB; }) &&
                     std::all_of(mem.end() - 64, mem.end(), [](uint8_t b) { return b == 0xAB; });
        CHECK_MSG(guard, "pattern %dx%d", w, h);
        RenderPlaceholder(img, "No video source", "Start a producer: vcam bars");
        guard = std::all_of(mem.begin(), mem.begin() + 64, [](uint8_t b) { return b == 0xAB; }) &&
                std::all_of(mem.end() - 64, mem.end(), [](uint8_t b) { return b == 0xAB; });
        CHECK_MSG(guard, "placeholder %dx%d", w, h);
        DrawText(img, -5, -3, 3, Rgb{255, 0, 0}, "clipped text ~!");
        guard = std::all_of(mem.begin(), mem.begin() + 64, [](uint8_t b) { return b == 0xAB; }) &&
                std::all_of(mem.end() - 64, mem.end(), [](uint8_t b) { return b == 0xAB; });
        CHECK_MSG(guard, "text %dx%d", w, h);
    }
    CHECK(TextWidth("ab", 2) == 22 && TextWidth("", 3) == 0 && TextHeight(2) == 14);
}

void WritePpm(const char* path, const ImageView& v)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", v.width, v.height);
    for (int y = 0; y < v.height; y++)
        for (int x = 0; x < v.width; x++) {
            const uint8_t* p = Px(v, x, y);
            const uint8_t rgb[3] = {p[2], p[1], p[0]};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

// --- benchmark (only with --bench) -------------------------------------------

void Bench()
{
    struct Case {
        PixelFormat in;
        int iw, ih;
        PixelFormat out;
        int ow, oh;
    };
    const Case cases[] = {
        {PixelFormat::BGRA, 1920, 1080, PixelFormat::NV12, 1920, 1080},
        {PixelFormat::BGRA, 2560, 1440, PixelFormat::NV12, 1920, 1080},
        {PixelFormat::BGRA, 3840, 2160, PixelFormat::NV12, 1920, 1080},
        {PixelFormat::BGRA, 1920, 1080, PixelFormat::NV12, 1280, 720},
        {PixelFormat::RGB24, 1280, 720, PixelFormat::NV12, 1920, 1080},
        {PixelFormat::NV12, 1920, 1080, PixelFormat::NV12, 1280, 720},
        {PixelFormat::NV12, 1920, 1080, PixelFormat::YUY2, 1920, 1080},
        {PixelFormat::NV12, 1920, 1080, PixelFormat::NV12, 1920, 1080},
        {PixelFormat::BGRA, 1920, 1080, PixelFormat::YUY2, 1920, 1080},
    };
    Converter conv;
    for (const Case& c : cases) {
        Buffer src = BlockyBgra(c.iw, c.ih, 1);
        Buffer in(c.in, c.iw, c.ih), out(c.out, c.ow, c.oh);
        conv.Convert(src.view(), in.span, ConvertOptions{});
        const int iters = 20;
        conv.Convert(in.view(), out.span, ConvertOptions{});
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; i++) conv.Convert(in.view(), out.span, ConvertOptions{});
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / iters;
        printf("  %-6s %4dx%-4d -> %-5s %4dx%-4d  %7.2f ms\n", FormatName(c.in), c.iw, c.ih, FormatName(c.out), c.ow,
               c.oh, ms);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        Bench();
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--render") == 0) {
        // Writes the test pattern and placeholder as PPM files for eyeballing.
        Buffer img(PixelFormat::BGRA, 1280, 720);
        const char* lines[] = {"VCAM TEST PATTERN", "1280x720 NV12 30 fps", "00:01:02.345  frame 1871"};
        RenderTestPattern(img.span, 1871, 62.345, lines, 3);
        WritePpm((std::string(argv[2]) + "/pattern.ppm").c_str(), img.view());
        RenderPlaceholder(img.span, "No video source", "Start a producer, e.g.  vcam bars");
        WritePpm((std::string(argv[2]) + "/placeholder.ppm").c_str(), img.view());
        return 0;
    }
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const TestCase& t : Registry()) {
        if (filter && !strstr(t.name, filter)) continue;
        const int before = g_failures;
        t.fn();
        printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", t.name);
        run++;
    }
    printf("%d tests, %d checks, %d failures\n", run, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
