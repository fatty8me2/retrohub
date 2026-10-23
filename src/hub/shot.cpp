#include "app.h"

/* ============================ main ============================ */

/* ---- headless screenshot harness (testing aid, no window is ever shown) ----
 * HUB_SCREENSHOT=<file.bmp>  renders with SDL's "dummy" video driver + software renderer, writes the frame, exits.
 * HUB_SHOT_SECS (default 4) wall-clock seconds to run first; HUB_SHOT_SIZE=WxH; HUB_SCALE=DPI scale (1, 1.5, 2);
 * HUB_SHOT_KEYS=right,right,down,enter,... keyboard-nav presses sent at 0.3s intervals before the shot;
 * HUB_SHOT_PAD=1 shows controller glyphs in the footer. */
void save_screenshot(SDL_Renderer* ren, const char* path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}
ImGuiKey shot_key(const std::string& n) {
    if (n == "right") return ImGuiKey_RightArrow;
    if (n == "left") return ImGuiKey_LeftArrow;
    if (n == "up") return ImGuiKey_UpArrow;
    if (n == "down") return ImGuiKey_DownArrow;
    if (n == "enter") return ImGuiKey_Enter;
    if (n == "esc") return ImGuiKey_Escape;
    if (n == "f2") return ImGuiKey_F2;
    if (n == "i") return ImGuiKey_I;
    if (n == "f") return ImGuiKey_F;
    return ImGuiKey_None;
}

