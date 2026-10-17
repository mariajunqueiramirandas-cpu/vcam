// Minimal software drawing into BGRA images: a built-in 5x7 font, anti-aliased
// shapes, the test pattern and the "no video source" placeholder. Portable.
#pragma once

#include <cstdint>

#include "vcam/convert.h"
#include "vcam/format.h"

namespace vcam {

// Text uses a 5x7 font in a 6x8 cell, scaled by an integer factor.
int TextWidth(const char* text, int scale);
int TextHeight(int scale);
void DrawText(const ImageSpan& bgra, int x, int y, int scale, Rgb color, const char* text);
// Text with a one-pixel-per-scale dark outline, readable on any background.
void DrawTextOutlined(const ImageSpan& bgra, int x, int y, int scale, Rgb color, const char* text);

// Blends a solid rectangle; opacity 0..255.
void FillRect(const ImageSpan& bgra, int x, int y, int w, int h, Rgb color, int opacity = 255);

// SMPTE-style colour bars with a luma ramp, a moving marker (shows motion and
// dropped frames), a border (shows cropping) and up to four lines of text.
void RenderTestPattern(const ImageSpan& bgra, uint64_t frame, double seconds, const char* const* lines,
                       int line_count);

// Dark card with a camera-off icon and two lines of text.
void RenderPlaceholder(const ImageSpan& bgra, const char* title, const char* subtitle);

}  // namespace vcam
