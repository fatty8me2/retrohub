#pragma once
/* Pure part of the external-window screenshot code (ext_media.cpp), moved out so it can be tested (hub_ext_media_logic_test). */
#include <cstddef>
#include <cstdint>

namespace extmedia {

inline bool all_black(const uint8_t* bgra, int w, int h, int stride) {
    for (int y = 0; y < h; y += 7) {
        const uint8_t* s = bgra + (size_t)y * stride;
        for (int x = 0; x < w; x += 5) if (s[x * 4] | s[x * 4 + 1] | s[x * 4 + 2]) return false;
    }
    return true;
}

}  // namespace extmedia
