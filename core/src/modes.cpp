#include "vcam/modes.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vcam/format.h"

namespace vcam {

std::vector<VideoMode> StandardModes(uint32_t max_width, uint32_t max_height, bool allow_60fps)
{
    static const VideoMode sizes[] = {
        {3840, 2160, 0}, {2560, 1440, 0}, {1920, 1080, 0}, {1600, 900, 0}, {1280, 720, 0},
        {960, 540, 0},   {848, 480, 0},   {640, 480, 0},   {640, 360, 0},  {424, 240, 0},
        {320, 240, 0},   {320, 180, 0},
    };
    std::vector<VideoMode> fitting;
    for (const VideoMode& s : sizes) {
        if (s.width <= max_width && s.height <= max_height) {
            fitting.push_back(s);
        }
    }
    if (fitting.empty()) {
        fitting.push_back({320, 180, 0});
    }

    // Default: the largest size not above 1080p (4K by default would cost every
    // app that just takes the first mode a lot of CPU for nothing).
    size_t def = 0;
    for (size_t i = 0; i < fitting.size(); i++) {
        if (fitting[i].width <= 1920 && fitting[i].height <= 1080) {
            def = i;
            break;
        }
    }

    std::vector<VideoMode> modes;
    auto add = [&](const VideoMode& s) {
        modes.push_back({s.width, s.height, 30});
        if (allow_60fps) {
            modes.push_back({s.width, s.height, 60});
        }
    };
    add(fitting[def]);
    for (size_t i = 0; i < fitting.size(); i++) {
        if (i != def) {
            add(fitting[i]);
        }
    }
    return modes;
}

bool ParseResolution(const char* text, uint32_t* width, uint32_t* height)
{
    if (!text) {
        return false;
    }
    std::string s;
    for (const char* p = text; *p; p++) {
        s += static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    }
    struct Named {
        const char* name;
        uint32_t w, h;
    };
    static const Named named[] = {
        {"4k", 3840, 2160},  {"2160p", 3840, 2160}, {"1440p", 2560, 1440}, {"1080p", 1920, 1080},
        {"720p", 1280, 720}, {"540p", 960, 540},    {"480p", 848, 480},    {"360p", 640, 360},
    };
    for (const Named& n : named) {
        if (s == n.name) {
            *width = n.w;
            *height = n.h;
            return true;
        }
    }
    const size_t x = s.find('x');
    if (x == std::string::npos || x == 0 || x + 1 >= s.size()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long w = strtoul(s.c_str(), &end, 10);
    if (end != s.c_str() + x) {
        return false;
    }
    const unsigned long h = strtoul(s.c_str() + x + 1, &end, 10);
    if (*end != '\0' || w < 16 || h < 16 || w > kMaxDimension || h > kMaxDimension) {
        return false;
    }
    *width = static_cast<uint32_t>(w);
    *height = static_cast<uint32_t>(h);
    return true;
}

}  // namespace vcam
