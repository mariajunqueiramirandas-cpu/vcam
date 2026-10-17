// The capture modes the virtual camera advertises. Portable.
#pragma once

#include <cstdint>
#include <vector>

namespace vcam {

struct VideoMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

// Standard 16:9 and 4:3 sizes no larger than max_width x max_height, each at
// 30 fps and (when allow_60fps) 60 fps. The first entry is the default mode:
// the largest size up to 1920x1080 at 30 fps.
std::vector<VideoMode> StandardModes(uint32_t max_width, uint32_t max_height, bool allow_60fps);

// Parses "1080p", "4k", "1440p", "720p" or "WxH". Returns false on bad input.
bool ParseResolution(const char* text, uint32_t* width, uint32_t* height);

}  // namespace vcam
