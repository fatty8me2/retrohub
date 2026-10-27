#pragma once
/* retrorun screen filters (G3) and procedural console bezels (G4). Pure CPU, no SDL, no ImGui: the frontend uploads the
 * result to a streaming texture. The default look (no filter, no bezel) never goes through this file.
 *
 *   Filters (all separable so a 1080p frame costs ~1 ms; see test_present_fx.cpp for the benchmark):
 *     crt   scanlines + RGB aperture mask + slight barrel bow + vignette, on top of a sharp-bilinear scale
 *     lcd   dot-matrix grid (GB / GBC / GBA look)
 *     dmg   original Game Boy green ramp + a light grid
 *     sharp sharp bilinear (crisp pixel centres, 1 px anti-aliased edges, good at non-integer scales)
 *   Bezels: a decorative frame for the pillarbox bars of a console, drawn with signed-distance shapes in the style of
 *   the boot art (src/hub/boot_art.cpp: soft shadow lip, top highlight, rounded panels, buttons). No licensed images. */
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fx {

enum class Filter { Off, Crt, Lcd, Dmg, Sharp };

bool parse_filter(const std::string& name, Filter* out);   // "off"|"none"|"crt"|"lcd"|"dmg"|"sharp" (case-insensitive)
const char* filter_name(Filter f);                         // "off", "crt", ...
const char* filter_label(Filter f);                        // "Off", "CRT", "LCD grid", "DMG green", "Sharp bilinear"
Filter next_filter(Filter f);                              // cycles Off -> CRT -> LCD -> DMG -> Sharp -> Off

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

/* A source image (ARGB/XRGB, 32-bit): a sub-rectangle of a frame is described by the pointer to its first pixel + stride. */
struct Src { const uint32_t* px = nullptr; int stride = 0; int w = 0, h = 0; };

/* Holds the per-size tables so a frame costs only the pixel work. Not thread safe (it uses its own worker threads internally). */
class Engine {
public:
    /* Scales `src` to dw x dh with `f` (f != Off) into out (resized to dw*dh, alpha forced to 255). */
    void render(Filter f, const Src& src, int dw, int dh, std::vector<uint32_t>* out);
    /* Bezel for the window ow x oh around the screen rect (opaque, the screen rect itself is black). Cached per argument set. */
    const std::vector<uint32_t>& bezel(const std::string& system, int ow, int oh, const Rect& screen);
    void set_threads(int n) { threads_ = n < 1 ? 1 : n; }   // default: min(4, hardware threads); 1 = single threaded

    struct Plan;   // opaque
private:
    std::shared_ptr<Plan> plan_;
    int threads_ = 0;
    std::vector<uint32_t> tint_;        // dmg: tinted copy of the source
    std::vector<uint32_t> bez_px_;
    std::string bez_sys_;
    int bez_ow_ = 0, bez_oh_ = 0;
    Rect bez_rect_;
};

}  // namespace fx
