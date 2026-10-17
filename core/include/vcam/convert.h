// Colour conversion and scaling between every vcam pixel format. Portable.
#pragma once

#include <cstdint>
#include <vector>

#include "vcam/format.h"

namespace vcam {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

struct ConvertOptions {
    ScaleMode scale = ScaleMode::Fit;
    bool mirror = false;  // flip left/right
    bool flip = false;    // flip top/bottom
    ColorSpace input_colorspace = ColorSpace::Bt601Limited;   // how YUV input is encoded
    ColorSpace output_colorspace = ColorSpace::Bt601Limited;  // how to encode YUV output
    Rgb background;                                           // padding colour for ScaleMode::Fit
};

// Where a src_w x src_h image lands in a dst_w x dst_h frame.
struct Placement {
    int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;  // rectangle written in the destination
    double src_x = 0, src_y = 0, src_w = 0, src_h = 0;  // window sampled from the source
};

// even_align keeps the destination rectangle on even coordinates, which chroma
// subsampled formats need so the padding does not bleed into the picture.
Placement ComputePlacement(int src_w, int src_h, int dst_w, int dst_h, ScaleMode mode, bool even_align);

// Fills the whole image with one colour.
bool FillImage(const ImageSpan& dst, Rgb color, ColorSpace colorspace);

// RGB <-> YUV for single values; used by tests and by FillImage.
void RgbToYuv(Rgb rgb, ColorSpace colorspace, uint8_t* y, uint8_t* u, uint8_t* v);
Rgb YuvToRgb(uint8_t y, uint8_t u, uint8_t v, ColorSpace colorspace);

// Converts and scales images. Keeps scratch buffers between calls, so reuse one
// instance per stream. Not thread-safe.
class Converter {
public:
    // Writes src into dst (any format to any format, any size to any size).
    // Returns false if either view is unusable; dst is untouched in that case.
    bool Convert(const ImageView& src, const ImageSpan& dst, const ConvertOptions& options);

private:
    ImageView Decode(const ImageView& src);
    bool Resample(const ImageView& canon, const Placement& placement, const ImageSpan& out, bool mirror,
                  bool flip);
    void ResamplePlane(const uint8_t* src, int src_pitch, int src_w, int src_h, int channels, double win_x,
                       double win_y, double win_w, double win_h, uint8_t* dst, int dst_pitch, int dst_w,
                       int dst_h, bool mirror, bool flip, bool force_alpha);

    std::vector<uint8_t> decoded_;
    std::vector<uint8_t> scaled_;
    std::vector<uint8_t> rgb_;
    std::vector<uint16_t> temp_;
    std::vector<int32_t> acc_;
    std::vector<int32_t> htaps_start_, vtaps_start_;
    std::vector<int16_t> htaps_w_, vtaps_w_;
    std::vector<int32_t> hpairs_;
    std::vector<int16_t> hw8_;
};

}  // namespace vcam
