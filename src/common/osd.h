#pragma once
/* On-screen display shared by the runners (retrorun, n64run): toast queue + one persistent corner slot, drawn with a tiny
 * built-in 5x7 bitmap font into an ARGB layer that each runner blends / uploads in its own renderer. No SDL, no GL, no deps.
 *
 *   osd::push("Saved state slot0", osd::Kind::Good);      // toast, default 2.5 s; newest 4 are shown, bottom-left
 *   osd::set_corner("REC 00:12", osd::Kind::Error);        // persistent, top-right; set_corner("") removes it
 *   osd::Layer L; bool changed;
 *   if (osd::render(&L, out_w, out_h, &changed)) { if (changed) upload(L); draw(L); }   // false: nothing visible
 *
 * Thread safe (the hub-control poll, key handlers and audio code may all push). Text is ASCII (other bytes draw as '?'). */
#include <cstdint>
#include <string>
#include <vector>

namespace osd {

enum class Kind { Info, Good, Warn, Error };

void push(const std::string& text, Kind kind = Kind::Info, double seconds = 2.5);
void set_corner(const std::string& text, Kind kind = Kind::Info);
/* Performance / online overlay (FPS, ping ...): one persistent line, top-left; set_stats("") removes it. Independent of the corner slot. */
void set_stats(const std::string& text);
void clear();                     // drop every toast and the corner text

/* The newest toast text still alive ("" if none): for runners that also mirror status into a window title. */
std::string latest_text();

struct Layer {
    int w = 0, h = 0;
    std::vector<uint32_t> px;     // ARGB8888, straight (non-premultiplied) alpha, 0 = transparent
};
/* Renders the visible items into *out, sized exactly out_w x out_h. Returns false when nothing is visible (out untouched).
 * *changed (optional) is false when the pixels are identical to the previous successful call with the same size, so a
 * caller that keeps a GPU texture can skip the upload. */
bool render(Layer* out, int out_w, int out_h, bool* changed = nullptr);

/* Source-over blend of `layer` onto an ARGB/XRGB frame (dst_w x dst_h pixels, stride in pixels). Clips to the smaller size. */
void blend(uint32_t* dst, int dst_w, int dst_h, int dst_stride_px, const Layer& layer);

/* Text metrics (pixels at the given integer scale) and the scale render() picks for a given output height. */
int text_width(const std::string& s, int scale);
int text_height(int scale);
int scale_for_height(int out_h);

/* Test hook: replace the millisecond clock (nullptr restores steady_clock). */
void set_clock_for_test(uint64_t (*now_ms)());

}  // namespace osd
