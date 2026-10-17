#include "simd.h"

#include <cstddef>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#define VCAM_SIMD_X64 1
#include <smmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace vcam::simd {
namespace {
bool g_enabled = true;
}

void SetEnabled(bool enabled)
{
    g_enabled = enabled;
}

#if VCAM_SIMD_X64

bool Available()
{
#if defined(_MSC_VER)
    static const bool supported = [] {
        int info[4];
        __cpuid(info, 1);
        return (info[2] & (1 << 19)) != 0;  // SSE4.1
    }();
#else
    static const bool supported = __builtin_cpu_supports("sse4.1");
#endif
    return supported && g_enabled;
}

namespace {

inline __m128i Load8(const void* p)
{
    return _mm_loadl_epi64(static_cast<const __m128i*>(p));
}

inline int32_t PackPair(int16_t a, int16_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(a)) |
                                (static_cast<uint32_t>(static_cast<uint16_t>(b)) << 16));
}

inline void Store4(uint8_t* out, __m128i v)
{
    const int32_t bits = _mm_cvtsi128_si32(v);
    memcpy(out, &bits, 4);
}

}  // namespace

void VerticalRow8(const uint8_t* const* rows, const int16_t* w, int n, int elems, uint8_t* out)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi32(1 << 13);
    int i = 0;
    for (; i + 8 <= elems; i += 8) {
        __m128i lo = round, hi = round;
        for (int k = 0; k < n; k += 2) {
            const bool pair = k + 1 < n;
            const __m128i a = _mm_unpacklo_epi8(Load8(rows[k] + i), zero);
            const __m128i b = pair ? _mm_unpacklo_epi8(Load8(rows[k + 1] + i), zero) : zero;
            const __m128i wv = _mm_set1_epi32(PackPair(w[k], pair ? w[k + 1] : 0));
            lo = _mm_add_epi32(lo, _mm_madd_epi16(_mm_unpacklo_epi16(a, b), wv));
            hi = _mm_add_epi32(hi, _mm_madd_epi16(_mm_unpackhi_epi16(a, b), wv));
        }
        lo = _mm_srai_epi32(lo, 14);
        hi = _mm_srai_epi32(hi, 14);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(out + i), _mm_packus_epi16(_mm_packs_epi32(lo, hi), zero));
    }
    for (; i < elems; i++) {
        int32_t acc = 1 << 13;
        for (int k = 0; k < n; k++) {
            acc += w[k] * rows[k][i];
        }
        out[i] = static_cast<uint8_t>(acc >> 14);
    }
}

void VerticalRow16(const uint16_t* const* rows, const int16_t* w, int n, int elems, uint8_t* out)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi32(1 << 20);
    int i = 0;
    for (; i + 8 <= elems; i += 8) {
        __m128i lo = round, hi = round;
        for (int k = 0; k < n; k += 2) {
            const bool pair = k + 1 < n;
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[k] + i));
            const __m128i b = pair ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[k + 1] + i)) : zero;
            const __m128i wv = _mm_set1_epi32(PackPair(w[k], pair ? w[k + 1] : 0));
            lo = _mm_add_epi32(lo, _mm_madd_epi16(_mm_unpacklo_epi16(a, b), wv));
            hi = _mm_add_epi32(hi, _mm_madd_epi16(_mm_unpackhi_epi16(a, b), wv));
        }
        lo = _mm_srai_epi32(lo, 21);
        hi = _mm_srai_epi32(hi, 21);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(out + i), _mm_packus_epi16(_mm_packs_epi32(lo, hi), zero));
    }
    for (; i < elems; i++) {
        int32_t acc = 1 << 20;
        for (int k = 0; k < n; k++) {
            acc += w[k] * rows[k][i];
        }
        out[i] = static_cast<uint8_t>(acc >> 21);
    }
}

namespace {

template <bool Out16>
void HorizontalBgra(const uint8_t* in, const int32_t* start, const int32_t* pairs, int n_pairs, int dst_w, void* out)
{
    const __m128i shuffle = _mm_setr_epi8(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);
    const __m128i zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi32(Out16 ? (1 << 6) : (1 << 13));
    for (int x = 0; x < dst_w; x++) {
        const uint8_t* p = in + static_cast<ptrdiff_t>(start[x]) * 4;
        const int32_t* wp = pairs + static_cast<ptrdiff_t>(x) * n_pairs;
        __m128i acc = round;
        for (int j = 0; j < n_pairs; j++) {
            // Two neighbouring pixels, channels interleaved: B0 B1 G0 G1 R0 R1 A0 A1.
            const __m128i px = _mm_unpacklo_epi8(_mm_shuffle_epi8(Load8(p + 8 * j), shuffle), zero);
            acc = _mm_add_epi32(acc, _mm_madd_epi16(px, _mm_set1_epi32(wp[j])));
        }
        if (Out16) {
            acc = _mm_srai_epi32(acc, 7);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(static_cast<uint16_t*>(out) + 4 * x),
                             _mm_packus_epi32(acc, acc));
        } else {
            acc = _mm_srai_epi32(acc, 14);
            Store4(static_cast<uint8_t*>(out) + 4 * x, _mm_packus_epi16(_mm_packs_epi32(acc, acc), zero));
        }
    }
}

template <bool Out16>
int HorizontalPlane(const uint8_t* in, int src_w, const int32_t* start, const int16_t* w8, int dst_w, void* out)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi32(Out16 ? (1 << 6) : (1 << 13));
    int done = 0;
    for (int x = 0; x + 4 <= dst_w; x += 4) {
        bool fits = true;
        for (int j = 0; j < 4; j++) {
            fits &= start[x + j] + 8 <= src_w;
        }
        if (!fits) {
            // Scalar for this group (only happens at the right edge of the source).
            for (int j = 0; j < 4; j++) {
                const uint8_t* p = in + start[x + j];
                const int16_t* w = w8 + static_cast<ptrdiff_t>(x + j) * 8;
                int32_t acc = Out16 ? (1 << 6) : (1 << 13);
                for (int k = 0; k < 8 && start[x + j] + k < src_w; k++) {
                    acc += w[k] * p[k];
                }
                if (Out16) {
                    static_cast<uint16_t*>(out)[x + j] = static_cast<uint16_t>(acc >> 7);
                } else {
                    static_cast<uint8_t*>(out)[x + j] = static_cast<uint8_t>(acc >> 14);
                }
            }
            done = x + 4;
            continue;
        }
        __m128i s[4];
        for (int j = 0; j < 4; j++) {
            const __m128i px = _mm_unpacklo_epi8(Load8(in + start[x + j]), zero);
            const __m128i wv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w8 + static_cast<ptrdiff_t>(x + j) * 8));
            s[j] = _mm_madd_epi16(px, wv);
        }
        __m128i t = _mm_hadd_epi32(_mm_hadd_epi32(s[0], s[1]), _mm_hadd_epi32(s[2], s[3]));
        t = _mm_add_epi32(t, round);
        if (Out16) {
            t = _mm_srai_epi32(t, 7);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(static_cast<uint16_t*>(out) + x), _mm_packus_epi32(t, t));
        } else {
            t = _mm_srai_epi32(t, 14);
            Store4(static_cast<uint8_t*>(out) + x, _mm_packus_epi16(_mm_packs_epi32(t, t), zero));
        }
        done = x + 4;
    }
    return done;
}

}  // namespace

void HorizontalBgra8(const uint8_t* in, const int32_t* start, const int32_t* pairs, int n_pairs, int dst_w,
                     uint8_t* out)
{
    HorizontalBgra<false>(in, start, pairs, n_pairs, dst_w, out);
}

void HorizontalBgra16(const uint8_t* in, const int32_t* start, const int32_t* pairs, int n_pairs, int dst_w,
                      uint16_t* out)
{
    HorizontalBgra<true>(in, start, pairs, n_pairs, dst_w, out);
}

int HorizontalPlane8(const uint8_t* in, int src_w, const int32_t* start, const int16_t* w8, int dst_w, uint8_t* out)
{
    return HorizontalPlane<false>(in, src_w, start, w8, dst_w, out);
}

int HorizontalPlane16(const uint8_t* in, int src_w, const int32_t* start, const int16_t* w8, int dst_w,
                      uint16_t* out)
{
    return HorizontalPlane<true>(in, src_w, start, w8, dst_w, out);
}

namespace {

struct ColorVectors {
    __m128i y, u, v;
};

inline ColorVectors MakeVectors(const YuvCoeffs15& c)
{
    return {
        _mm_setr_epi16(c.yb, c.yg, c.yr, 0, c.yb, c.yg, c.yr, 0),
        _mm_setr_epi16(c.ub, c.ug, c.ur, 0, c.ub, c.ug, c.ur, 0),
        _mm_setr_epi16(c.vb, c.vg, c.vr, 0, c.vb, c.vg, c.vr, 0),
    };
}

// Luma of 8 pixels given as four registers of two 16-bit pixels each.
inline __m128i Luma8(__m128i p01, __m128i p23, __m128i p45, __m128i p67, __m128i coeff, __m128i add)
{
    __m128i a = _mm_hadd_epi32(_mm_madd_epi16(p01, coeff), _mm_madd_epi16(p23, coeff));
    __m128i b = _mm_hadd_epi32(_mm_madd_epi16(p45, coeff), _mm_madd_epi16(p67, coeff));
    a = _mm_srai_epi32(_mm_add_epi32(a, add), 15);
    b = _mm_srai_epi32(_mm_add_epi32(b, add), 15);
    return _mm_packus_epi16(_mm_packs_epi32(a, b), _mm_setzero_si128());
}

// Sum of the two pixels held in a register (low 64 bits of the result).
inline __m128i PairSum(__m128i p)
{
    return _mm_add_epi16(p, _mm_srli_si128(p, 8));
}

// Four chroma values from four channel sums (two per register), shifted by `shift`.
inline __m128i Chroma4(__m128i q0, __m128i q1, __m128i coeff, __m128i add, int shift)
{
    __m128i r = _mm_hadd_epi32(_mm_madd_epi16(q0, coeff), _mm_madd_epi16(q1, coeff));
    r = _mm_sra_epi32(_mm_add_epi32(r, add), _mm_cvtsi32_si128(shift));
    return _mm_packus_epi16(_mm_packs_epi32(r, r), _mm_setzero_si128());
}

}  // namespace

int BgraToYuv420Rows(const uint8_t* r0, const uint8_t* r1, int w, uint8_t* y0, uint8_t* y1, uint8_t* u, uint8_t* v,
                     const YuvCoeffs15& c)
{
    const __m128i zero = _mm_setzero_si128();
    const ColorVectors cv = MakeVectors(c);
    const __m128i y_add = _mm_set1_epi32(c.y_add);
    const __m128i c_add = _mm_set1_epi32((128 << 17) + (1 << 16));
    int x = 0;
    for (; x + 8 <= w; x += 8) {
        const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + 4 * x));
        const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + 4 * x + 16));
        const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + 4 * x));
        const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + 4 * x + 16));
        const __m128i a01 = _mm_unpacklo_epi8(a0, zero), a23 = _mm_unpackhi_epi8(a0, zero);
        const __m128i a45 = _mm_unpacklo_epi8(a1, zero), a67 = _mm_unpackhi_epi8(a1, zero);
        const __m128i b01 = _mm_unpacklo_epi8(b0, zero), b23 = _mm_unpackhi_epi8(b0, zero);
        const __m128i b45 = _mm_unpacklo_epi8(b1, zero), b67 = _mm_unpackhi_epi8(b1, zero);

        _mm_storel_epi64(reinterpret_cast<__m128i*>(y0 + x), Luma8(a01, a23, a45, a67, cv.y, y_add));
        if (y1) {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(y1 + x), Luma8(b01, b23, b45, b67, cv.y, y_add));
        }

        // 2x2 sums: add the rows, then neighbouring pixels.
        const __m128i q0 = _mm_unpacklo_epi64(PairSum(_mm_add_epi16(a01, b01)), PairSum(_mm_add_epi16(a23, b23)));
        const __m128i q1 = _mm_unpacklo_epi64(PairSum(_mm_add_epi16(a45, b45)), PairSum(_mm_add_epi16(a67, b67)));
        const __m128i uu = Chroma4(q0, q1, cv.u, c_add, 17);
        const __m128i vv = Chroma4(q0, q1, cv.v, c_add, 17);
        if (v) {
            Store4(u + x / 2, uu);
            Store4(v + x / 2, vv);
        } else {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(u + x), _mm_unpacklo_epi8(uu, vv));
        }
    }
    return x;
}

int BgraToPacked422Row(const uint8_t* in, int w, uint8_t* out, bool uyvy, const YuvCoeffs15& c)
{
    const __m128i zero = _mm_setzero_si128();
    const ColorVectors cv = MakeVectors(c);
    const __m128i y_add = _mm_set1_epi32(c.y_add);
    const __m128i c_add = _mm_set1_epi32((128 << 16) + (1 << 15));
    int x = 0;
    for (; x + 8 <= w; x += 8) {
        const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 4 * x));
        const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 4 * x + 16));
        const __m128i a01 = _mm_unpacklo_epi8(a0, zero), a23 = _mm_unpackhi_epi8(a0, zero);
        const __m128i a45 = _mm_unpacklo_epi8(a1, zero), a67 = _mm_unpackhi_epi8(a1, zero);
        const __m128i yy = Luma8(a01, a23, a45, a67, cv.y, y_add);
        const __m128i q0 = _mm_unpacklo_epi64(PairSum(a01), PairSum(a23));
        const __m128i q1 = _mm_unpacklo_epi64(PairSum(a45), PairSum(a67));
        const __m128i uv = _mm_unpacklo_epi8(Chroma4(q0, q1, cv.u, c_add, 16), Chroma4(q0, q1, cv.v, c_add, 16));
        const __m128i packed = uyvy ? _mm_unpacklo_epi8(uv, yy) : _mm_unpacklo_epi8(yy, uv);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * x), packed);
    }
    return x;
}

#else  // no SIMD on this architecture

bool Available()
{
    return false;
}

void VerticalRow8(const uint8_t* const*, const int16_t*, int, int, uint8_t*) {}
void VerticalRow16(const uint16_t* const*, const int16_t*, int, int, uint8_t*) {}
void HorizontalBgra8(const uint8_t*, const int32_t*, const int32_t*, int, int, uint8_t*) {}
void HorizontalBgra16(const uint8_t*, const int32_t*, const int32_t*, int, int, uint16_t*) {}
int HorizontalPlane8(const uint8_t*, int, const int32_t*, const int16_t*, int, uint8_t*)
{
    return 0;
}
int HorizontalPlane16(const uint8_t*, int, const int32_t*, const int16_t*, int, uint16_t*)
{
    return 0;
}
int BgraToYuv420Rows(const uint8_t*, const uint8_t*, int, uint8_t*, uint8_t*, uint8_t*, uint8_t*, const YuvCoeffs15&)
{
    return 0;
}
int BgraToPacked422Row(const uint8_t*, int, uint8_t*, bool, const YuvCoeffs15&)
{
    return 0;
}

#endif

}  // namespace vcam::simd
