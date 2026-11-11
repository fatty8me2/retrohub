#pragma once
/* "Larger small text": the pixel size of a font face. Pure, unit tested in test_font_size.cpp.
 * Only the two small faces (11 px tiny, 15 px caption at scale 1) are raised, to 14 and 18; bigger faces never change.
 * The result is scaled by the UI scale like every other font, so layout scale is untouched. */
namespace hubfont {

inline float large_floor(float base_px) {
    if (base_px <= 11.5f) return 14.f;   // hints, chips
    if (base_px <= 15.5f) return 18.f;   // captions
    return base_px;
}

inline float font_px(float base_px, float ui_scale, bool large_text) {
    const float b = large_text && large_floor(base_px) > base_px ? large_floor(base_px) : base_px;
    return b * ui_scale;
}

}   // namespace hubfont
