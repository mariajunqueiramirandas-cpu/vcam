// SSE4.1 kernels used by the converter. Every CPU that runs Windows 11 has
// SSE4.1; elsewhere Available() checks at runtime. All kernels produce exactly
// the same results as the scalar code in convert.cpp.
#pragma once

#include <cstdint>

namespace vcam::simd {

bool Available();

// Turns SIMD off (tests compare both paths).
void SetEnabled(bool enabled);

// out[i] = (sum_k w[k] * rows[k][i] + round) >> 14, 8-bit input.
void VerticalRow8(const uint8_t* const* rows, const int16_t* w, int n, int elems, uint8_t* out);
// Same with 7 extra bits of precision in the input (>> 21).
void VerticalRow16(const uint16_t* const* rows, const int16_t* w, int n, int elems, uint8_t* out);

// BGRA horizontal pass; `pairs` holds each output's weights as packed int16
// pairs (n_pairs per output) and taps start at start[x]. 8-bit output (>> 14)
// or 16-bit output with 7 extra bits (>> 7).
void HorizontalBgra8(const uint8_t* in, const int32_t* start, const int32_t* pairs, int n_pairs, int dst_w,
                     uint8_t* out);
void HorizontalBgra16(const uint8_t* in, const int32_t* start, const int32_t* pairs, int n_pairs, int dst_w,
                      uint16_t* out);

// Single-channel horizontal pass for filters of up to 8 taps; w8 holds 8 int16
// weights per output (zero padded). Outputs whose 8-byte read would pass
// src_w are left to the caller; returns the number of outputs written from 0.
int HorizontalPlane8(const uint8_t* in, int src_w, const int32_t* start, const int16_t* w8, int dst_w,
                     uint8_t* out);
int HorizontalPlane16(const uint8_t* in, int src_w, const int32_t* start, const int16_t* w8, int dst_w,
                      uint16_t* out);

// 15-bit RGB -> YUV coefficients, laid out for the kernels.
struct YuvCoeffs15 {
    int16_t yb, yg, yr, ub, ug, ur, vb, vg, vr;
    int32_t y_add;
};

// Two BGRA rows (r1 may equal r0) to Y rows and one row of chroma (NV12 when
// v is null, else separate U and V). Returns the number of pixels done, a
// multiple of 8; the caller finishes the rest.
int BgraToYuv420Rows(const uint8_t* r0, const uint8_t* r1, int w, uint8_t* y0, uint8_t* y1, uint8_t* u,
                     uint8_t* v, const YuvCoeffs15& c);
// One BGRA row to YUY2 (uyvy = false) or UYVY. Same return convention.
int BgraToPacked422Row(const uint8_t* in, int w, uint8_t* out, bool uyvy, const YuvCoeffs15& c);

}  // namespace vcam::simd
