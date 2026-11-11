#pragma once
/* Pure part of the external-window screenshot code (ext_media.cpp), moved out so it can be tested (hub_ext_media_logic_test). */
#include <cstddef>
#include <cstdint>

namespace extmedia {

/* Was the captured frame (BGRA, alpha ignored) black? On "black" the caller throws the capture away and reports failure, so this errs on
 * KEEPING: the frame is black only when the brightest channel of EVERY pixel is at most kBlackMax (faint encoder noise). Any pixel above
 * that, however few, is picture, so fades, night scenes and loading screens are kept. Every pixel is scanned with an early exit at the
 * first picture pixel (a 4K black frame is a few milliseconds, small next to encoding a PNG). Padding bytes past each row are never read. */
constexpr int kBlackMax = 8;

inline bool all_black(const uint8_t* bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0) return true;
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = bgra + (size_t)y * stride;
        for (int x = 0; x < w; ++x, s += 4)
            if (s[0] > kBlackMax || s[1] > kBlackMax || s[2] > kBlackMax) return false;
    }
    return true;
}

}  // namespace extmedia
