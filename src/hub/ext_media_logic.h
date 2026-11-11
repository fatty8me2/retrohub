#pragma once
/* Pure part of the external-window screenshot code (ext_media.cpp), moved out so it can be tested (hub_ext_media_logic_test). */
#include <cstddef>
#include <cstdint>

namespace extmedia {

/* Was the captured frame (BGRA, alpha ignored) black? On "black" the caller throws the capture away (it falls back to another capture
 * method, then reports failure), so this errs on KEEPING: a pixel counts as picture only when its brightest channel is above
 * kLitLevel (dark scenes at 20 or more stay picture; noise and compression fuzz up to 16 do not), and the frame is black only when
 * fewer than 1 in kLitOneIn pixels is picture. The whole frame is scanned (every pixel, early exit on enough picture): a 4K frame is
 * about 8 million byte-triples, a few milliseconds, small next to encoding the PNG. A thin bright line or a letterboxed picture
 * therefore keeps the frame, and so does any tiny frame with a single lit pixel. Padding bytes past each row are never read. */
constexpr int kLitLevel = 16;
constexpr int kLitOneIn = 4000;

inline bool all_black(const uint8_t* bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0) return true;
    const uint64_t total = (uint64_t)w * (uint64_t)h;
    const uint64_t need = (total + kLitOneIn - 1) / kLitOneIn;   // lit pixels that make it "picture": at least 1, ceil(total / 4000)
    uint64_t lit = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = bgra + (size_t)y * stride;
        for (int x = 0; x < w; ++x, s += 4) {
            if (s[0] > kLitLevel || s[1] > kLitLevel || s[2] > kLitLevel) {
                if (++lit >= need) return false;
            }
        }
    }
    return true;
}

}  // namespace extmedia
