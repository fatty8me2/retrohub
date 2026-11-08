/* Coverflow state and the info panel under the carousel (included into libview_ui.cpp's anonymous namespace right before cases_nav.inc; see cases_view.inc).
 * Layout numbers adapted from PyroCade (MIT, Pyrodrifter). The panel sits bottom-left above the scrubber / hint bar: an accent rule, the eyebrow line
 * (console / position / media chip), the title (2 lines at most), the facts, a short description, the action buttons, and a 16:10 screenshot on the
 * right. Everything is cached when the selection (or the width) changes: nothing is built or allocated per frame in the draw path. */

struct CfState {
    hubui::cases::cf::EaseState pos_es;       // the glide of CS.pos.x (cubic-bezier .22 1 .36 1, 0.7 s)
    double idle_t = 0.0; float idle_w = 0.f; bool idle_rest = false;  // time since the carousel settled; the idle loop's weight (it fades in / out, never snaps)
    // the background colour grid (darkened, saturated): the one shown, the one it came from, the one it goes to
    ImU32 bg_cur[64] = {}, bg_from[64] = {}, bg_to[64] = {};
    float bg_t = 1.f; bool bg_init = false, bg_art = false; int bg_idx = -2; uint64_t bg_sig = ~0ull;
    ImU32 bg_tint = 0;
    // the info panel
    int info_focus = -2; uint64_t info_sig = ~0ull, info_gen = ~0ull; int info_tick = -1; float info_w = 0.f, info_fs = 0.f; bool info_tight = false;
    float info_t = 100.f;                     // seconds since the selection changed (drives the fade / rise)
    std::string eyebrow, chip, meta, title;
    std::vector<std::string> title_lines, desc_lines;
    ImU32 accent = 0;
    SDL_Texture* fake_pic = nullptr; int fake_w = 0, fake_h = 0; bool fake_tried = false;
    bool have_desc = false;
} CFX;

bool cases_flow_on() {
    static const bool force_classic = g_headless && std::getenv("HUB_SHOT_CASES_STYLE") && std::string(std::getenv("HUB_SHOT_CASES_STYLE")) == "classic";
    return !A.st.lib_cases_classic && !force_classic;
}

/* ---- background ---- */
ImU32 cf_bg_cell(ImU32 c) {   // blur(40px) saturate(1.3) brightness(.45) of one average colour
    const float r = (float)(c & 255), g = (float)((c >> 8) & 255), b = (float)((c >> 16) & 255);
    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    auto f = [&](float v) { return std::max(0, std::min(255, (int)((l + (v - l) * 1.3f) * 0.45f))); };
    return IM_COL32(f(r), f(g), f(b), 255);
}
/* Called every frame: starts a 1.1 s cross-fade when the selected cover (or its art arriving) changes, and mixes the grid that is shown. */
void cf_bg_update(int n, int cur_idx, uint64_t sig, ImU32 t0, ImU32 t1, bool no_art, float dt, bool reduced) {
    const uint32_t* bc = nullptr;
    if (n > 0 && !no_art) {
        const ArtRef a = art_of(cur_idx);
        SDL_Texture* tex = a.tex;
        if (sys_of(cur_idx) == hub::System::Switch) { int iw = 0, ih = 0; if (SDL_Texture* it = cases_shot_icon(&iw, &ih)) tex = it; }
        if (tex) bc = cases_art_blur(tex);
    }
    const bool have = bc != nullptr;
    if (!CFX.bg_init || CFX.bg_idx != cur_idx || CFX.bg_sig != sig || CFX.bg_art != have) {
        const bool first = !CFX.bg_init;
        std::memcpy(CFX.bg_from, CFX.bg_cur, sizeof CFX.bg_cur);
        for (int i = 0; i < 64; ++i) {
            if (bc) CFX.bg_to[i] = cf_bg_cell(bc[i]);
            else {   // no art: the console colours, top to bottom
                const ImU32 c = ui::mix(t0, t1, (float)(i / 8) / 7.f);
                CFX.bg_to[i] = cf_bg_cell(c);
            }
        }
        if (first) { std::memcpy(CFX.bg_cur, CFX.bg_to, sizeof CFX.bg_cur); std::memcpy(CFX.bg_from, CFX.bg_to, sizeof CFX.bg_to); }
        CFX.bg_t = first || reduced ? 1.f : 0.f;
        CFX.bg_init = true; CFX.bg_idx = cur_idx; CFX.bg_sig = sig; CFX.bg_art = have;
    }
    if (CFX.bg_t < 1.f) CFX.bg_t = reduced ? 1.f : std::min(1.f, CFX.bg_t + dt / 1.1f);
    const float k = hubui::cases::smoothstep01(CFX.bg_t);
    for (int i = 0; i < 64; ++i) CFX.bg_cur[i] = ui::mix(CFX.bg_from[i], CFX.bg_to[i], k);
}

